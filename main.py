# ShadowNet AEAR demo - single Pico W, two SX1278 chips
#   Chip 1 (SPI1) = Node A  : SENDER      (SOS packets)
#   Chip 2 (SPI0) = BaseStation : RECEIVER (forwards to webhook)
# The two chips talk over the AIR_TX / AIR_RX wires (simulated radio link).

from machine import Pin, SPI
import sx127x
import time
import gc
import network
import urequests
import json

# ---------------- LoRa parameters (from the paper, Table 1) ----------------
LORA_PARAMS = {
    'frequency': 868E6,
    'tx_power_level': 14,
    'signal_bandwidth': 125E3,
    'spreading_factor': 9,
    'coding_rate': 5,
    'preamble_length': 8,
    'implicitHeader': False,
    'sync_word': 0x12,
    'enable_CRC': False,
}

BACKEND_URL = "https://webhook.site/2be74210-605f-4521-a229-94156fc7765d"
SEND_INTERVAL_MS = 2000   # buffer retry interval


# ---------------- Hardware wrapper for the SX127x driver ----------------
class PicoLoRa(sx127x.SX127x):
    def __init__(self, spi, pins, parameters=None):
        self.spi = spi
        self.pin_ss = pins['ss']
        self.pin_reset = pins['reset']
        self.pin_RxDone = pins['dio_0']

        self.pin_ss.init(Pin.OUT, value=1)

        if self.pin_reset:
            self.pin_reset.init(Pin.OUT, value=1)
            self.pin_reset.value(0)
            time.sleep(0.01)
            self.pin_reset.value(1)
            time.sleep(0.01)

        if parameters:
            super().__init__(parameters=parameters)
        else:
            super().__init__()
        self.init()

    def transfer(self, pin_ss, address, value=0x00):
        response = bytearray(1)
        pin_ss.value(0)
        self.spi.write(bytearray([address]))
        if (address & 0x80):  # write
            self.spi.write(bytearray([value]))
        else:                 # read
            self.spi.write_readinto(bytearray([0x00]), response)
        pin_ss.value(1)
        return response

    def collect_garbage(self):
        gc.collect()          # silent (the stock driver prints memory stats)


# ---------------- Two radios, two separate SPI buses ----------------
# Node A (sender)   : SPI1  SCK=GP14 MOSI=GP15 MISO=GP12  NSS=GP13  DIO0=GP16
spi_a = SPI(1, baudrate=5000000, polarity=0, phase=0,
            sck=Pin(14), mosi=Pin(15), miso=Pin(12))
radio_a = PicoLoRa(spi_a, {'ss': Pin(13), 'reset': None, 'dio_0': Pin(16)}, LORA_PARAMS)

# BaseStation (receiver): SPI0  SCK=GP6 MOSI=GP7 MISO=GP4  NSS=GP5  DIO0=GP9
spi_b = SPI(0, baudrate=5000000, polarity=0, phase=0,
            sck=Pin(6), mosi=Pin(7), miso=Pin(4))
radio_b = PicoLoRa(spi_b, {'ss': Pin(5), 'reset': None, 'dio_0': Pin(9)}, LORA_PARAMS)


# ---------------- AEAR packet + scoring ----------------
def create_aear_packet(msg_id, dest, priority, ttl, visited_nodes, payload):
    packet = {
        "id": msg_id,
        "dest": dest,
        "priority": priority,      # 0 = Normal, 1 = Important, 2 = Emergency
        "ttl": ttl,                # max TTL is 12
        "visited": visited_nodes,  # prevents looping
        "data": payload,
    }
    return str(packet)


def calculate_routing_score(S_j, H_j, C_j, priority):
    if priority == 2:   # Emergency
        w_S, w_H, w_C = 0.4, 0.4, 0.2
    else:               # Normal
        w_S, w_H, w_C = 0.3, 0.3, 0.4
    return (w_S * S_j) + (w_H * H_j) + (w_C * C_j)


# ---------------- Start ----------------
print("\n--- SHADOWNET AEAR DEMO (single Pico, two LoRa chips) ---")
print("Node A (sender) and BaseStation (receiver) initialised.")

# BaseStation starts listening immediately
radio_b.receive()
print("[BaseStation] Listening for AEAR packets...")

# WiFi connects in the background; packets are still received meanwhile
wlan = None
try:
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    wlan.connect('Wokwi-GUEST', '')
except Exception as e:
    print("[BaseStation] WiFi unavailable:", e)
wifi_reported = False

msg_id = 101
next_send = time.ticks_add(time.ticks_ms(), 3000)

while True:
    now = time.ticks_ms()

    # --- WiFi status (report once) ---
    if wlan and (not wifi_reported) and wlan.isconnected():
        print("[BaseStation] WiFi connected, IP:", wlan.ifconfig()[0])
        wifi_reported = True

    # --- Node A: send an SOS packet every SEND_INTERVAL_MS ---
    if time.ticks_diff(now, next_send) >= 0:
        pkt = create_aear_packet(msg_id, "BaseStation", 2, 12, ["NodeA"], "SOS")
        radio_a.println(pkt)
        print("[NodeA] Sent AEAR packet:", pkt)
        msg_id += 1
        next_send = time.ticks_add(now, SEND_INTERVAL_MS)

    # --- BaseStation: check for a received packet ---
    if radio_b.receivedPacket():
        try:
            payload_bytes = radio_b.read_payload()
            rssi = radio_b.packetRssi()
            payload_str = payload_bytes.decode('utf-8').replace("'", '"')
            packet_data = json.loads(payload_str)
            packet_data["rssi"] = rssi

            print("[BaseStation] Intercepted LoRa packet (RSSI %d dBm): %s" % (rssi, packet_data))

            if wlan and wlan.isconnected():
                print("[BaseStation] Pushing to backend...")
                response = urequests.post(BACKEND_URL, json=packet_data)
                print("[BaseStation] Server response code:", response.status_code)
                response.close()
            else:
                print("[BaseStation] WiFi not connected yet - not forwarded")
        except Exception as e:
            print("[BaseStation] Error processing packet:", e)

        radio_b.receive()   # re-arm for the next packet

    time.sleep_ms(50)