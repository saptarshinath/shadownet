# ShadowNet: LoRa SOS Link Simulation (Wokwi)

A software simulation of the **communication layer** of ShadowNet, a decentralised emergency communication system. A sender node transmits an SOS packet over a simulated LoRa link, a base station receives it, and the base station forwards it to the internet through a webhook.

The project accompanies the research paper *"Adaptive Emergency-Aware Multi-Factor Routing for Infrastructure-Less LoRa Mesh Networks"* (AEAR), Heritage Institute of Technology, Kolkata. The paper has been submitted.

Because no hardware budget was available, the hardware part is simulated in [Wokwi](https://wokwi.com). Wokwi has no LoRa radio model, so this repository includes a **custom SX1278 chip model** and a wired link that stands in for the radio channel.

---

## Table of contents

1. [What this project does (and doesn't)](#1-what-this-project-does-and-doesnt)
2. [Architecture](#2-architecture)
3. [Circuit and wiring](#3-circuit-and-wiring)
4. [Packet format](#4-packet-format)
5. [How a packet is sent and received](#5-how-a-packet-is-sent-and-received)
6. [Repository files](#6-repository-files)
7. [Configuration](#7-configuration)
8. [Running the simulation](#8-running-the-simulation)
9. [Expected output](#9-expected-output)
10. [Troubleshooting](#10-troubleshooting)
11. [Limitations](#11-limitations)
12. [Moving to real hardware](#12-moving-to-real-hardware)
13. [Related paper](#13-related-paper)
14. [Authors](#14-authors)

---

## 1. What this project does (and doesn't)

**Does**
- Simulates an SOS packet travelling from a sender node (Node A) to a base station over LoRa, using the radio settings from the paper's Table 1 (868 MHz, 125 kHz bandwidth, SF9, coding rate 4/5, 14 dBm).
- Uses the same packet fields the paper defines: message ID, destination, priority, TTL and visited-node list.
- Runs the **real SX127x MicroPython driver** against a custom chip model, so the SPI protocol is the one real hardware uses.
- Forwards received packets to a webhook, with a small store-and-forward buffer and retries.

**Doesn't**
- It does **not** run the AEAR routing algorithm. A scoring function is included in `main.py` but is not called, because a single hop has no next-hop decision to make.
- It does **not** model the physical radio: airtime, collisions, interference, CRC or real RSSI. RSSI is a constant (-80 dBm).
- The performance results in the paper come from a separate network-level simulation, not from this project.

---

## 2. Architecture

```
                         ONE Raspberry Pi Pico W (MicroPython)
        +--------------------------------------------------------------+
        |                          main.py                             |
        |      Node A role (sender)        BaseStation role (receiver) |
        +-----------+-------------------------------+------------------+
                    | SPI1                          | SPI0
                    v                               v
             +-------------+     AIR_TX -> AIR_RX     +-------------+
             |  SX1278 #1  | ======================>  |  SX1278 #2  |
             |  (Node A)   | <======================  | (BaseStn)   |
             +-------------+     AIR_RX <- AIR_TX     +-------------+
                       simulated radio channel

   BaseStation --Wi-Fi--> HTTPS POST --> webhook.site (or your backend)
```

Why a single Pico with two chips? In Wokwi, only one of two MicroPython boards ran the program, so the receiver never started. Putting both nodes on one board, each with its own SPI bus, avoids that.

---

## 3. Circuit and wiring

**Parts**
- 1x Raspberry Pi Pico W (MicroPython v1.21.0 build, selected in `diagram.json`)
- 2x SX1278 LoRa module (custom chip, `chip1` and `chip2`)

**Pin connections**

| Signal | Node A, chip 1 (sender) | BaseStation, chip 2 (receiver) | Purpose |
|---|---|---|---|
| SPI bus | SPI1 | SPI0 | Separate buses, no shared lines |
| SCK | GP14 | GP6 | Clock from Pico to chip |
| MOSI | GP15 | GP7 | Data from Pico to chip (addresses and values) |
| MISO | GP12 | GP4 | Data from chip to Pico (register reads) |
| NSS (chip select) | GP13 | GP5 | Low selects the chip, high ignores it |
| DIO0 | GP16 | GP9 | Chip output, high on packet received. Wired, but the code polls registers instead of reading it |
| VCC | 3V3 | 3V3 | Power |
| GND | GND.1 | GND.2 | Ground |

**Simulated air link (simulation only)**

| From | To | Purpose |
|---|---|---|
| `chip1:AIR_TX` | `chip2:AIR_RX` | Node A to BaseStation |
| `chip2:AIR_TX` | `chip1:AIR_RX` | BaseStation to Node A |

Not connected: `DIO1` and the module's RESET pin (the code passes `reset=None`).

---

## 4. Packet format

### Application packet

Built by `create_aear_packet()` in `main.py` and sent as text:

```
{'priority': 2, 'ttl': 12, 'data': 'SOS', 'visited': ['NodeA'], 'id': 101, 'dest': 'BaseStation'}
```

| Field | Meaning | Value in this demo |
|---|---|---|
| `id` | Unique message ID, for duplicate and loop detection | Starts at 101, +1 per packet |
| `dest` | Destination node | `BaseStation` |
| `priority` | 0 = normal, 1 = important, 2 = emergency | 2 |
| `ttl` | Maximum forwarding lifetime | 12 |
| `visited` | Nodes the packet has passed through | `['NodeA']` |
| `data` | Payload | `SOS` |

The text is 97 bytes. On receipt the base station adds an `rssi` field before forwarding.

### Air frame (between the two chips)

| Byte | Content |
|---|---|
| 0 | `0xA5` start marker |
| 1 | Sync word (`0x12`) |
| 2 | Payload length |
| 3 onward | Payload bytes |

The frame is carried as a 115200-baud UART on the AIR_TX / AIR_RX wires. A real LoRa frame (preamble, header, payload) would look different.

### SPI transactions (Pico to chip)

Every register access is two bytes with NSS held low:
- **Write:** `address | 0x80`, then the value.
- **Read:** `address`, then a dummy `0x00`, during which the chip returns the register value on MISO.

SPI settings: 5 MHz, mode 0 (polarity 0, phase 0).

---

## 5. How a packet is sent and received

### Sending (Node A)
1. `main.py` builds the packet and calls `radio_a.println(packet)`.
2. The driver puts the chip in standby, resets the FIFO pointer (`0x0D`) and payload length (`0x22`).
3. It writes the packet bytes one by one to the FIFO register (`0x00`), then writes the payload length.
4. It writes `0x83` (LoRa mode + TX) to the mode register `0x01`.
5. The chip model wraps the bytes in an air frame and sends it on `AIR_TX`, returns to standby and sets the TX-done flag (`0x08`).

### Receiving (BaseStation)
1. At start-up, `radio_b.receive()` puts the chip in continuous receive mode (`0x85`).
2. The chip reads the air frame. If it is in an RX mode and the sync word matches, it copies the payload into its FIFO, sets the length register (`0x13`), RSSI register (`0x1A`) and the RX-done flag (`0x40`), and raises DIO0. Otherwise it prints `frame dropped`.
3. `receivedPacket()` polls the flags register. When it reads exactly `0x40`, a packet is waiting.
4. `read_payload()` reads the bytes from the FIFO. `packetRssi()` converts register `0x1A` to dBm (77 - 157 = -80).
5. The bytes are decoded, parsed as JSON, tagged with `rssi`, and added to the pending buffer.
6. The base station posts the oldest pending packet to the backend. It removes the packet from the buffer only after a success response, otherwise it retries after 8 seconds.

---

## 6. Repository files

| File | Role |
|---|---|
| `main.py` | Application. Defines the two radios, builds and sends SOS packets, receives them, buffers them, and pushes them to the backend. |
| `sx127x.py` | Standard SX127x MicroPython LoRa driver (register map, `init`, `println`, `receive`, `receivedPacket`, `read_payload`, `packetRssi`). Used unchanged. |
| `sx1278.chip.c` | Custom Wokwi chip: a simplified SX1278 with a 128-byte register file, 256-byte FIFO, the two-byte SPI protocol, TX/RX modes, IRQ flags, sync-word check, and the AIR_TX / AIR_RX frame link. |
| `sx1278.chip.json` | Pin definition for the custom chip: NSS, DIO0, DIO1, SCK, MOSI, MISO, GND, VCC, AIR_TX, AIR_RX. |
| `diagram.json` | The circuit: one Pico W, two SX1278 chips and all the wires listed in section 3. |

Names in `sx1278.chip.json`, `sx1278.chip.c` and `diagram.json` (for example the part type `chip-sx1278` and the pin names) must match exactly.

### `main.py` in brief
- **Settings:** `LORA_PARAMS` (the paper's radio settings), `BACKEND_URL`, `SEND_INTERVAL_MS = 2000`, `MAX_PENDING = 10`, `PUSH_RETRY_MS = 8000`.
- **`PicoLoRa`:** wrapper that connects the driver to the Pico's SPI bus and chip-select pin. Its `transfer()` implements the two-byte SPI protocol.
- **Main loop (every 50 ms):** report Wi-Fi status, send an SOS every 2 seconds, poll the receiver, buffer received packets, push to the backend with retry.
- **Timers** use `time.ticks_ms()` instead of blocking sleeps, so receiving continues while waiting to send.

### `sx1278.chip.c` in brief
- Models registers, FIFO, operating modes, IRQ flags (write-1-to-clear), and the version register (`0x42` returns `0x12`).
- Does **not** model airtime, collisions, CRC, or matching of frequency or spreading factor.
- Prints status lines (`TX`, `RX`, `entered RX mode`, `frame dropped`) to the Wokwi Chips Console.

---

## 7. Configuration

| Setting | Where | Default |
|---|---|---|
| Frequency / bandwidth / SF / coding rate / TX power | `LORA_PARAMS` in `main.py` | 868 MHz / 125 kHz / SF9 / 4/5 / 14 dBm |
| Sync word | `LORA_PARAMS` | `0x12` |
| Send interval | `SEND_INTERVAL_MS` | 2000 ms |
| Buffer size | `MAX_PENDING` | 10 packets |
| Retry delay | `PUSH_RETRY_MS` | 8000 ms |
| Backend endpoint | `BACKEND_URL` | A webhook.site URL, replace with your own |
| Wi-Fi network | `network.WLAN.connect(...)` | `Wokwi-GUEST`, no password (Wokwi's simulated network) |
| Pin assignments | `main.py` and `diagram.json` | See section 3 |

**Backend URL:** get your own endpoint at https://webhook.site and paste it into `BACKEND_URL`. Do not commit a personal URL to a public repository, because anyone who has it can read and post to it.

Note: the 868 MHz setting follows the paper's Table 1, where it is marked as a simulation assumption. License-free LoRa frequencies differ by country, so check your local regulations before using real hardware.

---

## 8. Running the simulation

1. Go to [wokwi.com](https://wokwi.com) and create a new **Raspberry Pi Pico W** project with **MicroPython**.
2. Add or replace these files in the project: `main.py`, `sx127x.py`, `sx1278.chip.c`, `sx1278.chip.json`, `diagram.json`.
3. Wokwi compiles the custom chip from the `.chip.c` and `.chip.json` files. Confirm that the two chips appear in the diagram with their pins (including AIR_TX and AIR_RX).
4. Open your own endpoint at https://webhook.site and set `BACKEND_URL` in `main.py`.
5. Press the green **Start** button.
6. Watch the **Serial Monitor** tab for application output and the **Chips Console** tab for chip-level output.

The Wokwi for VS Code extension needs additional setup (a `wokwi.toml`, a compiled `.wasm` chip, and `mpremote`). The browser editor is the simplest route.

---

## 9. Expected output

**Serial Monitor**
```
--- SHADOWNET AEAR DEMO (single Pico, two LoRa chips) ---
Node A (sender) and BaseStation (receiver) initialised.
[BaseStation] Listening for AEAR packets...
[NodeA] Sent AEAR packet: {'priority': 2, 'ttl': 12, 'data': 'SOS', 'visited': ['NodeA'], 'id': 101, 'dest': 'BaseStation'}
[BaseStation] Intercepted LoRa packet (RSSI -80 dBm): {'id': 101, 'ttl': 12, 'rssi': -80, ...}
[BaseStation] WiFi connected, IP: 10.10.0.1
[BaseStation] Backend response 200 for packet id 101
```

**Chips Console**
```
[SX1278] chip ready
[SX1278] first SPI access: an MCU is talking to this chip
[SX1278] entered RX mode (op_mode=0x85)
[SX1278] TX request: len_reg=97 fifo_ptr=97 tx_base=0
[SX1278] TX 97 bytes
[SX1278] RX 97 bytes
```

On the webhook.site page, each packet appears as a POST with a JSON body (`id`, `priority`, `ttl`, `visited`, `data`, `dest`, `rssi`).

---

## 10. Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| `frame dropped (not in RX mode ...)` | The receiving chip was not put in receive mode. Check that the BaseStation calls `radio_b.receive()` at start-up. |
| `TX 0 bytes` or a doubled FIFO pointer | An older version of `sx1278.chip.c`. Use the current file, where each SPI transaction is exactly one address byte plus one data byte. |
| No `RX` lines at all | Check that `chip1:AIR_TX` is wired to `chip2:AIR_RX` and the reverse, and that the pin names match the `.chip.json`. |
| Server response `404` | The webhook URL has expired. Get a new one from webhook.site. |
| `MBEDTLS_ERR_SSL_CONN_EOF` | The HTTPS connection closed unexpectedly (low memory on the Pico or the server dropping the connection). `main.py` frees memory before each post and retries. Try a fresh URL, or the `http://` form if the service accepts it. |
| Only one board runs | Wokwi did not run both MicroPython boards in one project. This is why the project uses one Pico with two chips. |

---

## 11. Limitations

- Single hop only. No multi-hop forwarding and no AEAR next-hop selection in the demo.
- The radio is a software model: constant RSSI, no airtime or duty-cycle limits, no collisions, no CRC.
- The packet is JSON text (97 bytes). The paper assumes 32-byte packets, and a real LoRa deployment would use a compact binary format. At SF9 and 125 kHz a 97-byte packet takes roughly 0.5 s on air by the standard LoRa formula.
- The parser replaces single quotes with double quotes to read the packet, which would fail if the payload contained an apostrophe.
- `calculate_routing_score()` uses example weights and is not connected to the demo.
- The 2-second send interval would exceed typical duty-cycle limits on real 868 MHz hardware.

---

## 12. Moving to real hardware

The packet format, SPI protocol, driver and register settings are the ones real SX127x hardware uses, so the migration is mostly physical:

1. Replace the two custom chips with real SX1278 modules (for example Ra-02 boards) and one module per node.
2. Keep the SPI wiring (SCK, MOSI, MISO, NSS) and add the RESET line. Power the module from 3.3 V only.
3. Attach an antenna matched to the frequency before powering the module.
4. Choose a frequency allowed in your region and update `LORA_PARAMS`. The bundled driver's `setFrequency` accepts only a fixed list (169, 433, 434, 866, 868 and 915 MHz), so other frequencies need a driver change.
5. Replace the `AIR_TX` / `AIR_RX` wires with the radio itself (they are not needed).
6. Run `main.py` as separate sender and receiver programs, one per Pico.

---

## 13. Related paper

**Adaptive Emergency-Aware Multi-Factor Routing for Infrastructure-Less LoRa Mesh Networks.**
AEAR scores each candidate neighbour using normalised signal strength (S), hop efficiency (H) and congestion availability (C):

```
Score(n_j, p) = w_S(P)·S_j + w_H(P)·H_j + w_C(P)·C_j,   w_S + w_H + w_C = 1
```

where the weights depend on packet priority P (0 normal, 1 important, 2 emergency). The packet carries a message ID, a visited-node record, a TTL and a destination, and unreachable paths trigger store-and-forward buffering. Forwarding is local and costs O(d) per decision, where d is the number of candidate neighbours.

This repository demonstrates the link layer and packet structure that the routing sits on top of.

---

## 14. Authors

Nilanjana Dutta, Shubham Kumar Gupta, Saptarshi Nath and Ayush Kumar, Heritage Institute of Technology, Kolkata, India.

---

## License

Add a license of your choice (for example MIT) before publishing. `sx127x.py` is a third-party driver, so check and keep its original license and attribution.
