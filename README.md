# LoRa Messenger

Custom firmware for LilyGO T-Deck and Heltec WiFi LoRa 32 V3 boards with direct LoRa text messaging, mesh routing, and ACK-based reliable delivery.

## What It Does

- Sends messages directly between devices over LoRa.
- Uses the T-Deck built-in keyboard and touch UI when building the `tdeck` environment.
- Uses USB serial input and a compact OLED status display when building the `heltec_v3` environment.
- Displays a small chat log on T-Deck, or radio/route/status data on Heltec V3.
- Retransmits each outgoing message until an ACK is received or retries are exhausted.
- Suppresses duplicate received messages, so a retry does not show the same text repeatedly.
- Relays encrypted messages through nearby nodes using TTL-limited flooding.
- Shows mesh packet, route, node, and AODV status on a dedicated dashboard.
- BLE transport is currently disabled by default so the LoRa path can be stabilized first.

This is intentionally peer-to-peer and serverless. The first version broadcasts data packets and accepts an ACK from the receiving peer, which is ideal for testing with two T-Decks.

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

The device name is included inside the encrypted message payload. The encryption key is used to derive a 256-bit AES key, and outgoing chat payloads are encrypted with AES-CTR before transmission. Both devices must use the same encryption key to read each other's messages.

The keyboard/serial command fallback is still available. Type a command into the message input or serial monitor and press Enter:

```text
/id
/settings
/freq 915.0
/bw 125
/sf 10
/cr 6
/pwr 22
/name FieldNode
/key shared-secret
/defaults
/to all
/to A1B2C3D4
/discover A1B2C3D4
/routes
/help
```

Use `/id` on each board when checking identity collisions. It prints the full eFuse MAC, derived mesh node id, generated default name, and the name currently saved in flash. The mesh node id is derived from the full eFuse MAC, not just the low 32 bits.

Settings are saved in flash and restored after reboot. Both devices must use matching LoRa settings to communicate.

Supported values:

- Frequency: `150.0` to `960.0` MHz, limited by your module and local law
- Bandwidth: nearest supported value to `7.8`, `10.4`, `15.6`, `20.8`, `31.25`, `41.7`, `62.5`, `125`, `250`, or `500` kHz
- Spreading factor: `5` to `12`
- Coding rate: `5` to `8`
- TX power: `-9` to `22` dBm
- Device name: up to 16 characters
- Encryption key: up to 32 characters

## Heltec V3 Operation

The Heltec V3 build has no keyboard or large chat screen. The OLED is a status panel showing:

- local node id
- radio frequency and spreading factor
- selected destination
- active route and BLE-nearby counts
- RX/TX/forward counters
- latest AODV and send status

Use the serial monitor at `115200` baud for chat and configuration. On USB CDC serial, plain text lines are sent as chat messages. On the guarded UART0 console, use `/send <message>` for chat. Slash commands configure the device and are persisted in flash, including `/freq`, `/bw`, `/sf`, `/cr`, `/pwr`, `/name`, and `/key`.

## Reliability Model

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

- One message is pending at a time.
- Before transmitting, the radio waits a short randomized backoff and runs SX1262 channel activity detection.
- If another LoRa preamble is detected, the packet stays queued and retries after a longer randomized backoff.
- Group-chat receivers delay ACKs randomly; once a node hears another ACK for the same message, it suppresses its own ACK.
- The sender retries every `CHAT_ACK_TIMEOUT_MS`.
- The sender gives up after `CHAT_MAX_RETRIES`.
- The receiver sends an ACK even for duplicate packets.
- Duplicate message ids from the same sender are not shown again.

## Mesh Routing

The routing layer is an AODV-style low-bandwidth mesh:

- HELLO beacons advertise direct neighbors every 60 seconds.
- Route tables store destination, next hop, hop count, and last-seen time.
- `/to <node-id>` selects a unicast destination and starts discovery if needed.
- RREQ packets are broadcast with a short TTL.
- The destination replies with RREP along the reverse route.
- Unicast DATA and ACK packets are forwarded by next hop.
- `/routes` shows the current route table.
- `/to all` returns to broadcast mode.

The mesh dashboard shows:

- local node id and selected destination
- BLE status, currently shown as disabled in the default LoRa-only build
- active route count and last HELLO age
- RX/TX packet totals
- DATA, ACK, HELLO, RREQ, and RREP counters
- forwarded packet count
- latest AODV event
- active routes with destination, next hop, hop count, and age

## Bluetooth Mesh Transport

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

Broadcast behavior:

- Each outgoing chat message starts with TTL `3`.
- Each node displays a new message once, then schedules one relay after a small random delay.
- Relayed packets preserve the original source node and message id.
- Each relay decrements TTL. Packets stop when TTL reaches `1`.
- Nodes keep a small duplicate cache, so repeated packets do not get displayed or relayed again.
- ACKs are still lightweight local delivery signals; chat payloads remain encrypted across relays and routed hops.

This is still tuned for small low-rate LoRa groups. Beacons and discovery traffic cost airtime, so keep message rates modest.

Tune these in `platformio.ini`:

```ini
-DCHAT_MAX_RETRIES=8
-DCHAT_ACK_TIMEOUT_MS=2500
```

## Hardware Notes

The `tdeck` environment uses LilyGO T-Deck pins for:

- SX1262 LoRa radio
- ST7789 display
- I2C keyboard at address `0x55`

The T-Deck needs the optional LoRa module installed.

The `heltec_v3` environment uses the PlatformIO `heltec_wifi_lora_32_V3` board definition for:

- SX1262 LoRa radio on the built-in SPI pins
- SSD1306 OLED on the built-in OLED I2C pins
- USB serial for all chat input and configuration
