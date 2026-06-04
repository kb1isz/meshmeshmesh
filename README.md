# LoRa Messenger

> **AI-Assisted Development** — This firmware was developed with the assistance of large language models (Claude, Cline) as a collaborative coding tool. The human author (kb1isz) directed the architecture, reviewed all changes, and made final decisions.

Custom firmware for LilyGO T-Deck and Heltec WiFi LoRa 32 V3 boards with direct LoRa text messaging, mesh routing, ACK-based reliable delivery, and a full suite of mesh networking features.

## Table of Contents

- [What It Does](#what-it-does)
- [Features](#features)
  - [Core Chat](#core-chat)
  - [Mesh Routing](#mesh-routing)
  - [Reliability Model](#reliability-model)
  - [Encryption](#encryption)
  - [Message Fragmentation](#message-fragmentation)
  - [Store-and-Forward](#store-and-forward)
  - [Emergency Messages](#emergency-messages)
  - [Node Roles](#node-roles)
  - [Contact List / Presence](#contact-list--presence)
  - [Frequency Hopping FHSS](#frequency-hopping-fhss)
  - [Bluetooth Mesh Transport](#bluetooth-mesh-transport)
  - [GPS Position Sharing](#gps-position-sharing)
  - [Persistent Chat History](#persistent-chat-history)
  - [Battery Monitoring](#battery-monitoring)
- [Build And Flash](#build-and-flash)
- [Build Configuration](#build-configuration)
- [Region Frequency](#region-frequency)
- [Slash Commands](#slash-commands)
- [Heltec V3 Operation](#heltec-v3-operation)
- [Wire Packet Format](#wire-packet-format)
- [Hardware Notes](#hardware-notes)
- [License](#license)

## What It Does

- Sends messages directly between devices over LoRa.
- Supports **group chat** (broadcast to all nodes) and **direct chat** (unicast to a specific node).
- Uses the T-Deck built-in keyboard, touch UI, and 320x240 color LCD when building the `tdeck` environment.
- Uses USB serial input and a compact 128x64 OLED status display when building the `heltec_v3` environment.
- Displays chat logs with bubble-style messages on T-Deck, or radio/route/status data on Heltec V3.
- Retransmits each outgoing message until an ACK is received or retries are exhausted.
- Suppresses duplicate received messages, so a retry does not show the same text repeatedly.
- Relays encrypted messages through nearby nodes using TTL-limited flooding.
- Shows mesh packet, route, node, AODV, and FHSS status on a dedicated dashboard.
- BLE transport is currently disabled by default so the LoRa path can be stabilized first.

This is intentionally peer-to-peer and serverless.

## Features

### Core Chat

- **Group chat** — broadcast messages to all nodes. Every node receives and displays the message once, then schedules one relay with a random delay to flood the mesh.
- **Direct chat** — unicast messages to a specific node identified by its 8-hex-digit node ID. The mesh routing layer discovers and maintains routes automatically.
- **Chat tabs** — switch between Group and Direct chat tabs on the T-Deck screen. The Direct tab cycles through active conversations.
- **Character counter** — shows typed character count (e.g. `13/43`) in the input field.
- **Message display** — outgoing messages appear in navy bubbles (right-aligned), incoming in maroon bubbles (left-aligned). Messages wider than ~48 characters wrap across multiple rows, rendered bottom-up so the latest content is always visible.

### Mesh Routing

The routing layer is an AODV-style low-bandwidth mesh:

| Feature | Description |
|---------|-------------|
| HELLO beacons | Advertise direct neighbors every 60 seconds. Contain device name and FHSS sync data. |
| Route tables | Store destination, next hop, hop count, and last-seen time. Support up to 16 entries. |
| Route discovery | `/to <node-id>` selects a unicast destination and broadcasts RREQ if no route exists. |
| RREQ/RREP | Route requests broadcast with short TTL. Destination replies with RREP along the reverse route. |
| Route forwarding | Unicast DATA and ACK packets are forwarded by next-hop through intermediate nodes. |
| Route expiry | Entries expire after 5 minutes without refresh. |
| Route discovery timeout | Gives up after 30 seconds. |

Leaf nodes (see [Node Roles](#node-roles)) update routes but never forward or relay.

### Reliability Model

Each data packet contains:

- source node id
- destination node id
- next-hop node id
- previous-hop node id
- message id
- packet type
- TTL
- body length
- CRC16 over the application packet

Chat message contents are encrypted before being placed in the data packet. ACK packets contain no chat text.

Delivery behavior:

- One message is pending at a time (per-node serialized delivery).
- Before transmitting, the radio waits a short randomized backoff and runs SX1262 channel activity detection (CCA).
- If another LoRa preamble is detected, the packet stays queued and retries after a longer randomized backoff (180-900ms).
- **Group ACK suppression** — Group-chat receivers delay ACKs randomly (120-850ms). Once a node hears another ACK for the same message, it suppresses its own ACK to reduce airtime.
- The sender retries every `CHAT_ACK_TIMEOUT_MS` (default 2500ms).
- The sender gives up after `CHAT_MAX_RETRIES` (default 8 attempts).
- The receiver sends an ACK even for duplicate packets (to confirm delivery to the sender).
- Duplicate message ids from the same sender are not shown again (32-slot deduplication buffer).

Tune these in `platformio.ini`:

```ini
-DCHAT_MAX_RETRIES=8
-DCHAT_ACK_TIMEOUT_MS=2500
```

### Encryption

- **AES-256-CTR** encryption using the ESP32 built-in mbedtls library.
- The encryption key is derived from the user-provided passphrase via **SHA-256**.
- Wire format (before hex encoding): `[16 bytes nonce][ciphertext...]`
- Plaintext format before encryption: `[sender name, max 16 chars]\n[message, max 43 chars]` for single-packet messages, or up to 340 message characters when fragmentation is enabled.
- The nonce is generated from the ESP32 hardware RNG (`esp_fill_random`).
- The hex-encoded encrypted payload is placed in the packet body.
- Both devices must use the same encryption key to read each other's messages.

> **Security note**: AES-CTR provides confidentiality but no authentication/integrity protection. Physical access to the device allows reading the key from NVS flash. Enable ESP32 flash encryption for production use. A future version should migrate to AES-GCM or AES-CTR+HMAC.

### Message Fragmentation

Messages longer than a single encrypted packet can carry are automatically split across up to 8 fragments:

- The full message is encrypted first, then split into binary fragments.
- Each fragment carries a hex-encoded header (total fragments, fragment index, and base message ID) followed by hex-encoded encrypted data.
- Each fragment gets its own packet `messageId` for ACK/retry, while the base message ID in the fragment header lets the receiver reassociate them.
- Fragments are delivered through the normal ACK/retry delivery pipeline.
- Receiving end collects fragments into assembly slots (up to 4 concurrent assemblies), concatenates them, then decrypts the full payload.
- Fragment assembly times out after 30 seconds.

Maximum message length: 340 characters.

Fragmentation counters are visible on the mesh dashboard (Frag RX / Assembled / Timeout).

Enabled by default (`-DENABLE_MESSAGE_FRAGMENTATION=1`).

### Store-and-Forward

Messages that fail delivery after exhausting all retries are automatically promoted to a long-term flash-backed queue:

- Failed messages are stored in NVS flash in the `store_fwd` namespace.
- Up to 20 messages can be stored simultaneously.
- Stored messages survive device reboot.
- Retried every 60 seconds, up to 60 retries (~1 hour).
- **Triggered retry** — when a HELLO or route discovery indicates a previously-unreachable node is back online, stored messages destined for that node are retried immediately.
- Delivery status is shown in the chat log (e.g. "sfwd delivered: Hello from earlier").
- Stored message counters appear on the mesh dashboard (S-F).

Enabled by default (`-DENABLE_STORE_FORWARD=1`).

### Emergency Messages

Priority distress messages with special handling:

- **Bypass the normal queue** — Emergency messages are inserted at the front of the pending queue, displacing the oldest non-emergency message if the queue is full.
- **Fast retries** — Retry every 1 second (vs 2.5s for normal messages), up to 20 attempts.
- **Red styling** — Displayed in bright red (`COLOR_ALERT = 0xF800`) in the chat log with `*** EMERGENCY ***` prefix.
- **Relay priority** — Emergency messages are forwarded immediately with only a 50ms delay (vs 250-900ms for normal flood relay).
- **Leaf nodes still relay** — Even leaf nodes forward emergency messages.
- Packet type: `PACKET_TYPE_EMERGENCY = 7`.

Enabled by default (`-DENABLE_EMERGENCY_MSG=1`).

To send: use the `/emergency` slash command or send a PACKET_TYPE_EMERGENCY via serial.

### Node Roles

Battery-saving mode via role-based behavior:

| Role | Behavior |
|------|----------|
| **Relay** (default) | Full mesh participation. Forwards data, relays HELLOs, participates in route discovery. |
| **Leaf** | No forwarding, no relay, no route request handling. Only sends/receives direct messages and HELLOs. Updates routes but never forwards. Still relays emergency messages. |

The role is persisted in flash alongside radio settings. Status is shown on the mesh dashboard.

Enabled by default (`-DENABLE_NODE_ROLES=1`).

### Contact List / Presence

- Automatically discovered contacts from HELLO beacons and route replies.
- Up to 16 contact entries stored in memory.
- Each contact tracks: node ID, name, last seen time, RSSI, and online/offline status.
- Contacts marked offline after 2× route TTL (10 minutes) without contact.
- Contact query/reply packet types allow explicit directory discovery.
- Online contact count displayed on the mesh dashboard.

Enabled by default (`-DENABLE_CONTACTS=1`).

### Frequency Hopping FHSS

Epoch-based time-slotted frequency hopping for improved spectrum utilization and resistance to interference. Enabled by default in the `tdeck` build (`-DENABLE_FREQ_HOPPING=1`).

**How it works:**

- A set of channels is computed from the ISM band (902-928 MHz US, configured via `HOP_BAND_MIN_MHZ`/`HOP_BAND_MAX_MHZ`) divided by the configured bandwidth.
- Channels are shuffled using the encryption key as a seed (so all devices with the same key share the same hop sequence).
- Each device computes the current hop slot from a synchronized network time:
  ```
  networkTime = millis() + hopEpochOffset
  hopSlot = (networkTime / HOP_INTERVAL_MS) % HOP_COUNT
  ```
- `hopEpochOffset` aligns each device's local millis() to the network epoch, computed from HELLO beacon sync data.
- The hop interval is computed dynamically from LoRa airtime: `max(2000ms, 2 × maxPacketAirtime)`.

**Boot sync:** A booting device rendezvouses on channel 0, sending non-authoritative discovery HELLOs and listening for authoritative SYNC HELLO replies from synced devices. Unsynced discovery HELLOs are ignored for time sync, so two joining devices cannot accidentally sync to each other's local boot clocks. If no authoritative SYNC HELLO is heard before `HOP_RENDEZVOUS_TIMEOUT_MS`, the device cold-starts a new epoch on channel 0 and broadcasts an authoritative SYNC HELLO so a fresh network can form.

**TX/RX gating:**
- One TX per slot (prevents flooding a single channel).
- No TX in the slot following an RX (gives other devices a clean window).
- Proactive HELLOs on the rendezvous channel ensure booting devices hear at least one HELLO per full cycle.

**FHSS dashboard display:**
- SYNCED/NOT SYNCED status
- Current slot and channel count
- Current frequency
- Epoch offset in ms
- Network time
- Time since last sync

Disabled by default in the `heltec_v3` build. The `tdeck` build has it enabled.

### Bluetooth Mesh Transport

BLE mesh transport is currently compiled off with:

```ini
-DENABLE_BLE_MESH=0
```

The code is still present for later work, but the default firmware does not initialize NimBLE, advertise, scan, or send packets over BLE. All chat, ACK, route discovery, HELLO, and relay packets use LoRa.

When BLE is re-enabled, this section applies.

Each device advertises a small BLE manufacturer beacon containing:
- project marker
- node id
- device name

Every 45 seconds, the device scans briefly for nearby T-Deck beacons. BLE-discovered nodes appear on the mesh dashboard with node id, device name, RSSI, and age.

BLE-discovered nodes are also added as one-hop routes. The firmware exposes a BLE GATT mesh packet characteristic, so nearby nodes can exchange the same packet format used over LoRa:
- chat DATA
- ACK
- HELLO
- RREQ
- RREP

If a packet has a BLE-nearby next hop, the firmware tries BLE first. Broadcast packets are sent to nearby BLE nodes and over LoRa. LoRa remains the long-range fallback; BLE helps nearby nodes exchange traffic faster and with less LoRa airtime.

### GPS Position Sharing

GPS telemetry via I2C GPS modules (disabled by default):

- Position data format: `lat:xx.xxxxxx,lon:yyy.yyyyyy,alt:####`
- Broadcast every 5 minutes.
- Requires hardware-specific GPS module and the `TinyGPS++` library.
- Enable by setting `-DENABLE_GPS_TELEMETRY=1` in build flags.

### Persistent Chat History

Chat logs are automatically saved to NVS flash every 30 seconds and restored after reboot:

- Group chat buffer (last 8 messages) is persisted.
- Direct chat buffers (up to 6 conversations) are persisted with node ID and name.
- Survives device reboot and power loss.

Enabled by default (`-DENABLE_PERSISTENT_HISTORY=1`).

### Battery Monitoring

- Battery voltage reading from ADC (GPIO 4 on T-Deck, GPIO 1 on Heltec V3).
- Read every 30 seconds.
- LiPo voltage range: 3.20V (0%) to 4.15V (100%).
- Displayed in the status bar and on the mesh dashboard.
- Color-coded: green (>50%), orange (20-50%), red (≤20%).

## Build And Flash

Install PlatformIO, then from this directory:

T-Deck:

```sh
pio run -e tdeck
pio run -e tdeck --target upload
pio device monitor -e tdeck
```

Heltec WiFi LoRa 32 V3:

```sh
pio run -e heltec_v3
pio run -e heltec_v3 --target upload
pio device monitor -e heltec_v3
```

Always include `-e heltec_v3` when uploading Heltec firmware. Running `pio run --target upload` without an environment uploads the default `tdeck` build.

If the monitor is blank, make sure you are using the Heltec environment explicitly:

```sh
pio device monitor -e heltec_v3 --baud 115200
```

Press the board `RST` button after opening the monitor. The Heltec build prints boot messages immediately and then a `[status]` line about every 15 seconds.

The Heltec build prints debug/status output to both USB CDC serial and UART0 by default. UART0 input is guarded: it accepts slash commands such as `/settings`, `/freq`, `/bw`, `/name`, and `/key`, but ignores plain non-command lines so floating UART pins cannot accidentally send chat messages. To send a chat message from the UART0 console, use `/send <message>`.

If upload fails, hold the trackball middle button while plugging in USB to enter download mode.

## Build Configuration

| Flag | Default | Description |
|------|---------|-------------|
| `RADIO_FREQ_MHZ` | `915.0` | Operating frequency in MHz |
| `CHAT_MAX_RETRIES` | `8` | Maximum ACK retry attempts per message |
| `CHAT_ACK_TIMEOUT_MS` | `2500` | Time between retries (ms) |
| `ENABLE_BLE_MESH` | `0` | Enable BLE mesh transport |
| `ENABLE_FREQ_HOPPING` | `0` (1 for tdeck) | Enable frequency hopping FHSS |
| `ENABLE_MESSAGE_FRAGMENTATION` | `1` | Enable long-message fragmentation |
| `ENABLE_STORE_FORWARD` | `1` | Enable flash-backed message queue |
| `ENABLE_PERSISTENT_HISTORY` | `1` | Enable chat history persistence |
| `ENABLE_CONTACTS` | `1` | Enable contact list/presence |
| `ENABLE_EMERGENCY_MSG` | `1` | Enable emergency messages |
| `ENABLE_GPS_TELEMETRY` | `0` | Enable I2C GPS position sharing |
| `ENABLE_NODE_ROLES` | `1` | Enable relay/leaf node roles |
| `HOP_BAND_MIN_MHZ` | `902.0` | FHSS band lower bound (MHz) |
| `HOP_BAND_MAX_MHZ` | `928.0` | FHSS band upper bound (MHz) |
| `HOP_RENDEZVOUS_TIMEOUT_MS` | `180000` | FHSS wait-for-existing-network timeout before cold-starting a new epoch (ms) |
| `HELTEC_ENABLE_UART0_OUTPUT` | `1` | Mirror serial output to UART0 |
| `HELTEC_ENABLE_UART0_INPUT` | `1` | Accept guarded input from UART0 |

**FHSS note:** When `ENABLE_FREQ_HOPPING=1`, the hop interval is computed dynamically from LoRa airtime, so `HOP_INTERVAL_MS` is a fallback only. The band range defaults to 902-928 MHz (US ISM 915 MHz band). For EU operation, set `HOP_BAND_MIN_MHZ=863.0` and `HOP_BAND_MAX_MHZ=870.0`.

## Region Frequency

The default build flag is:

```ini
-DRADIO_FREQ_MHZ=915.0
```

Use the legal LoRa frequency for your region and hardware module. Common starting points are:

- `915.0` MHz for US modules
- `868.0` MHz for EU modules
- `433.0` MHz for 433 MHz modules

You can also change LoRa settings directly on the device:

- Tap the `M` button near the top-right corner to open the mesh dashboard.
- Tap the gear icon in the top-right corner.
- Use the `LoRa` and `Security` tabs at the top of the settings screen.
- Tap the frequency row to open the numeric keypad, then tap `OK`.
- Tap the bandwidth, spreading factor, coding rate, or TX power rows to cycle through available values.
- On the `Security` tab, tap `Name` or `Key`, then type with the physical keyboard.
- Tap `Save` to apply the new settings, save them in flash, and return to chat.
- Tap `Cancel` to leave without applying changes.

The physical keyboard also works on the frequency keypad. Pressing Enter on the settings screen saves.

Settings are saved in flash and restored after reboot. Both devices must use matching LoRa settings to communicate.

Supported values:

- Frequency: `150.0` to `960.0` MHz, limited by your module and local law
- Bandwidth: nearest supported value to `7.8`, `10.4`, `15.6`, `20.8`, `31.25`, `41.7`, `62.5`, `125`, `250`, or `500` kHz
- Spreading factor: `5` to `12`
- Coding rate: `5` to `8`
- TX power: `-9` to `22` dBm
- Device name: up to 16 characters
- Encryption key: up to 32 characters

## Slash Commands

Type a command into the message input or serial monitor and press Enter:

| Command | Description |
|---------|-------------|
| `/id` | Print full identity: eFuse MAC, mesh node ID, default name, saved name |
| `/settings` | Show current radio settings summary |
| `/freq <MHz>` | Set frequency (e.g. `/freq 915.0`) |
| `/bw <kHz>` | Set bandwidth (e.g. `/bw 125`) |
| `/sf <value>` | Set spreading factor (5-12) |
| `/cr <value>` | Set coding rate (5-8) |
| `/pwr <dBm>` | Set TX power (-9 to 22) |
| `/name <text>` | Set device name (up to 16 chars) |
| `/key <text>` | Set encryption key (up to 32 chars) |
| `/defaults` | Reset all settings to defaults |
| `/to all` | Return to broadcast/group mode |
| `/to <node-id>` | Select unicast destination, start route discovery if needed |
| `/discover <node-id>` | Force route discovery for a specific node |
| `/routes` | Show current route table |
| `/frag <message>` | Force fragmented delivery for a long message |
| `/emergency <message>` | Send an emergency priority message |
| `/send <message>` | Send a chat message (UART0 serial only) |
| `/role relay` | Set node role to relay (full mesh participation) |
| `/role leaf` | Set node role to leaf (battery saving, no forwarding) |
| `/help` | Show available commands |

Use `/id` on each board when checking identity collisions. It prints the full eFuse MAC, derived mesh node id, generated default name, and the name currently saved in flash. The mesh node id is derived from the full eFuse MAC using SplitMix64, not just the low 32 bits.

## Heltec V3 Operation

The Heltec V3 build has no keyboard or large chat screen. The OLED is a 128x64 status panel showing:

- local node id
- radio frequency and spreading factor
- radio status (ok/off)
- selected destination and active route/BLE-nearby counts
- RX/TX/forward packet counters
- latest AODV event
- status line
- battery voltage and percent
- pending queue depth and HELLO age

Use the serial monitor at `115200` baud for chat and configuration. On USB CDC serial, plain text lines are sent as chat messages. On the guarded UART0 console, use `/send <message>` for chat. Slash commands configure the device and are persisted in flash.

## Wire Packet Format

```
Byte layout (MAX_PACKET_LEN = 180 bytes total):

Offset  Size  Field          Description
------  ----  -----          -----------
  0      1     magic[0]       'T' (0x54)
  1      1     magic[1]       'D' (0x44)
  2      1     version        PACKET_VERSION (1)
  3      1     type           Packet type (1-10)
  4      4     source         32-bit LE source node ID
  8      4     destination    32-bit LE destination node ID
 12      4     messageId      32-bit LE message ID
 16      1     bodyLen        Length of body payload (0..MAX_BODY_LEN)
 17      1     ttl            Time-to-live (decremented each hop)
 18      4     nextHop        32-bit LE next-hop node ID
 22      4     previousHop    32-bit LE previous-hop node ID
 26    var     body           Payload (bodyLen bytes)
 178     2     crc16          CRC-16/CCITT over header + body
```

Packet types:

| Type | Value | Description |
|------|-------|-------------|
| DATA | 1 | Chat message payload |
| ACK | 2 | Delivery acknowledgment |
| HELLO | 3 | Neighbor discovery beacon |
| RREQ | 4 | AODV route request |
| RREP | 5 | AODV route reply |
| FRAGMENT | 6 | Multi-packet message fragment |
| EMERGENCY | 7 | Emergency/priority message |
| POSITION | 8 | GPS/position beacon |
| CONTACT_QUERY | 9 | Contact directory query |
| CONTACT_REPLY | 10 | Contact directory reply |

## Mesh Dashboard

Accessible from the chat screen by tapping the `M` button near the top-right corner. Shows:

- Local node ID and selected destination
- Active route count and BLE node count
- HELLO beacon age
- RX/TX packet totals
- DATA, ACK, HELLO, RREQ, and RREP counters
- Forwarded packet count
- FHSS status (slot, frequency, sync state, epoch, network time)
- Feature counters (fragments, emergency, store-and-forward, contacts)
- Battery voltage and percent
- BLE status and nearby BLE nodes (when enabled)
- Active routes with destination, next hop, hop count, and age

## Hardware Notes

The `tdeck` environment uses LilyGO T-Deck pins for:

- SX1262 LoRa radio
- ST7789 display
- I2C keyboard at address `0x55`
- GT911 capacitive touch controller

The T-Deck needs the optional LoRa module installed.

The `heltec_v3` environment uses the PlatformIO `heltec_wifi_lora_32_V3` board definition for:

- SX1262 LoRa radio on the built-in SPI pins
- SSD1306 OLED on the built-in OLED I2C pins
- USB serial for all chat input and configuration

## License

This project is licensed under the MIT License. See the LICENSE file for details.
