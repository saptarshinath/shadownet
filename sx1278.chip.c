// Wokwi custom chip: simplified SX1278 (LoRa mode) with a wired "air" link.
//
// SPI side : behaves like an SX127x register file + FIFO (what sx127x.py uses).
// Air side : AIR_TX / AIR_RX pins carry frames over a UART. Wire chip1.AIR_TX
//            to chip2.AIR_RX and chip2.AIR_TX to chip1.AIR_RX; that wire pair
//            stands in for the radio channel.
//
// Frame on the air: [0xA5][sync word][length][payload...]
//
// SPDX-License-Identifier: MIT

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG_FIFO            0x00
#define REG_OP_MODE         0x01
#define REG_FIFO_ADDR_PTR   0x0D
#define REG_FIFO_TX_BASE    0x0E
#define REG_FIFO_RX_BASE    0x0F
#define REG_FIFO_RX_CURRENT 0x10
#define REG_IRQ_FLAGS       0x12
#define REG_RX_NB_BYTES     0x13
#define REG_PKT_RSSI        0x1A
#define REG_PKT_SNR         0x1B
#define REG_PAYLOAD_LENGTH  0x22
#define REG_SYNC_WORD       0x39
#define REG_VERSION         0x42

#define IRQ_TX_DONE 0x08
#define IRQ_RX_DONE 0x40

#define MODE_STDBY 0x01
#define MODE_TX    0x03
#define MODE_RX_CONTINUOUS 0x05
#define MODE_RX_SINGLE     0x06

#define AIR_START 0xA5

typedef struct {
  pin_t cs;
  pin_t dio0;
  spi_dev_t spi;
  uart_dev_t uart;

  // SPI transaction state
  uint8_t spi_buf[1];
  bool addr_phase;
  bool txn_done;
  bool seen_spi;
  uint32_t tx_count;
  bool is_write;
  uint8_t addr;

  // SX127x memory
  uint8_t regs[128];
  uint8_t fifo[256];

  // air-interface receive parser
  int rx_state;
  uint8_t rx_sync;
  uint8_t rx_len;
  uint8_t rx_idx;
  uint8_t rx_buf[256];

  // air-interface transmit buffer
  uint8_t tx_frame[3 + 256];
} chip_state_t;

static void update_dio0(chip_state_t *c) {
  pin_write(c->dio0, (c->regs[REG_IRQ_FLAGS] & IRQ_RX_DONE) ? HIGH : LOW);
}

static void transmit(chip_state_t *c) {
  uint8_t len = c->regs[REG_PAYLOAD_LENGTH];
  uint8_t base = c->regs[REG_FIFO_TX_BASE];
  uint8_t ptr = c->regs[REG_FIFO_ADDR_PTR];
  printf("[SX1278] TX request: len_reg=%d fifo_ptr=%d tx_base=%d\n", len, ptr, base);
  if (len == 0) {
    len = (uint8_t)(ptr - base);  // fallback: bytes actually written into the FIFO
  }
  c->tx_frame[0] = AIR_START;
  c->tx_frame[1] = c->regs[REG_SYNC_WORD];
  c->tx_frame[2] = len;
  for (int i = 0; i < len; i++) {
    c->tx_frame[3 + i] = c->fifo[(uint8_t)(base + i)];
  }
  uart_write(c->uart, c->tx_frame, 3 + len);
  c->tx_count++;
  printf("[SX1278] TX %d bytes\n", len);
}

static uint8_t read_reg(chip_state_t *c, uint8_t a) {
  if (a == REG_FIFO) {
    uint8_t v = c->fifo[c->regs[REG_FIFO_ADDR_PTR]];
    c->regs[REG_FIFO_ADDR_PTR]++;
    return v;
  }
  if (a == REG_VERSION) return 0x12;
  return c->regs[a];
}

static void write_reg(chip_state_t *c, uint8_t a, uint8_t v) {
  switch (a) {
    case REG_FIFO:
      c->fifo[c->regs[REG_FIFO_ADDR_PTR]] = v;
      c->regs[REG_FIFO_ADDR_PTR]++;
      break;
    case REG_IRQ_FLAGS:            // write 1 to clear
      c->regs[REG_IRQ_FLAGS] &= (uint8_t)~v;
      update_dio0(c);
      break;
    case REG_OP_MODE:
      if ((v & 0x07) == MODE_TX) {
        transmit(c);
        c->regs[REG_OP_MODE] = (v & 0xF8) | MODE_STDBY;  // back to standby after TX
        c->regs[REG_IRQ_FLAGS] |= IRQ_TX_DONE;
      } else {
        uint8_t old = c->regs[REG_OP_MODE];
        c->regs[REG_OP_MODE] = v;
        if ((v & 0x07) >= MODE_RX_CONTINUOUS && (v & 0x07) <= MODE_RX_SINGLE &&
            (old & 0x07) != (v & 0x07)) {
          printf("[SX1278] entered RX mode (op_mode=0x%02X)\n", v);
        }
      }
      break;
    default:
      c->regs[a] = v;
  }
}

static void deliver(chip_state_t *c) {
  uint8_t mode = c->regs[REG_OP_MODE] & 0x07;
  if (mode != MODE_RX_CONTINUOUS && mode != MODE_RX_SINGLE) {
    printf("[SX1278] frame dropped (not in RX mode, op_mode=0x%02X; this chip has itself sent %u frames)\n", c->regs[REG_OP_MODE], (unsigned)c->tx_count);
    return;
  }
  if (c->rx_sync != c->regs[REG_SYNC_WORD]) {
    printf("[SX1278] frame dropped (sync word mismatch)\n");
    return;
  }
  uint8_t base = c->regs[REG_FIFO_RX_BASE];
  for (int i = 0; i < c->rx_len; i++) {
    c->fifo[(uint8_t)(base + i)] = c->rx_buf[i];
  }
  c->regs[REG_FIFO_RX_CURRENT] = base;
  c->regs[REG_RX_NB_BYTES] = c->rx_len;
  c->regs[REG_PKT_RSSI] = 77;   // 77 - 157 = -80 dBm as read by sx127x.py at 868 MHz
  c->regs[REG_PKT_SNR] = 36;    // 36 * 0.25 = +9 dB
  c->regs[REG_IRQ_FLAGS] |= IRQ_RX_DONE;
  update_dio0(c);
  printf("[SX1278] RX %d bytes\n", c->rx_len);
}

static void on_air_byte(void *user_data, uint8_t byte) {
  chip_state_t *c = (chip_state_t *)user_data;
  switch (c->rx_state) {
    case 0:
      if (byte == AIR_START) c->rx_state = 1;
      break;
    case 1:
      c->rx_sync = byte;
      c->rx_state = 2;
      break;
    case 2:
      c->rx_len = byte;
      c->rx_idx = 0;
      if (byte == 0) {
        deliver(c);
        c->rx_state = 0;
      } else {
        c->rx_state = 3;
      }
      break;
    case 3:
      c->rx_buf[c->rx_idx++] = byte;
      if (c->rx_idx >= c->rx_len) {
        deliver(c);
        c->rx_state = 0;
      }
      break;
  }
}

static void on_air_write_done(void *user_data) {
  (void)user_data;
}

static void on_spi_done(void *user_data, uint8_t *buffer, uint32_t count) {
  chip_state_t *c = (chip_state_t *)user_data;
  if (c->txn_done) {
    return;  // transaction already complete: ignore stray/extra callbacks
  }
  if (c->addr_phase) {
    // first byte of the transaction = address (+ write flag)
    c->is_write = (buffer[0] & 0x80) != 0;
    c->addr = buffer[0] & 0x7F;
    c->addr_phase = false;
    buffer[0] = c->is_write ? 0x00 : read_reg(c, c->addr);  // data byte to shift out next
    spi_start(c->spi, buffer, count);                        // arm the data byte
  } else {
    // second byte = data; transaction ends here
    if (c->is_write) write_reg(c, c->addr, buffer[0]);
    c->txn_done = true;
  }
}

static void on_cs_change(void *user_data, pin_t pin, uint32_t value) {
  chip_state_t *c = (chip_state_t *)user_data;
  if (value == LOW) {
    if (!c->seen_spi) {
      c->seen_spi = true;
      printf("[SX1278] first SPI access: an MCU is talking to this chip\n");
    }
    c->addr_phase = true;
    c->txn_done = false;
    c->spi_buf[0] = 0x00;
    spi_start(c->spi, c->spi_buf, 1);
  } else {
    spi_stop(c->spi);
  }
}

void chip_init(void) {
  chip_state_t *c = (chip_state_t *)calloc(1, sizeof(chip_state_t));

  // register defaults
  c->regs[REG_OP_MODE] = 0x80 | MODE_STDBY;
  c->regs[0x1D] = 0x72;
  c->regs[0x1E] = 0x70;
  c->regs[REG_SYNC_WORD] = 0x12;

  c->cs = pin_init("NSS", INPUT_PULLUP);
  c->dio0 = pin_init("DIO0", OUTPUT_LOW);

  const pin_watch_config_t cs_watch = {
    .edge = BOTH,
    .pin_change = on_cs_change,
    .user_data = c,
  };
  pin_watch(c->cs, &cs_watch);

  const spi_config_t spi_cfg = {
    .sck = pin_init("SCK", INPUT),
    .mosi = pin_init("MOSI", INPUT),
    .miso = pin_init("MISO", INPUT),
    .mode = 0,
    .done = on_spi_done,
    .user_data = c,
  };
  c->spi = spi_init(&spi_cfg);

  const uart_config_t uart_cfg = {
    .tx = pin_init("AIR_TX", OUTPUT_HIGH),
    .rx = pin_init("AIR_RX", INPUT_PULLUP),
    .baud_rate = 115200,
    .rx_data = on_air_byte,
    .write_done = on_air_write_done,
    .user_data = c,
  };
  c->uart = uart_init(&uart_cfg);

  printf("[SX1278] chip ready\n");
}