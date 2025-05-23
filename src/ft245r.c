/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2003-2004 Theodore A. Roth <troth@openavr.org>
 * some code:
 * Copyright (C) 2011-2012 Roger E. Wolff <R.E.Wolff@BitWizard.nl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* ft245r -- FT245R/FT232R Synchronous BitBangMode Programmer
  default pin assign
               FT232R/FT245R
  sdi   = 1;  # RxD  /D1
  sck   = 0;  # RTS  /D0
  sdo   = 2;  # TxD  /D2
  reset = 4;  # DTR  /D4
*/

/*
 * The ft232r is very similar, or even "identical" in the synchronous
 * bitbang mode that we use here.
 *
 * This allows boards that have an ft232r for communication and an avr as the
 * processor to function as their own "ICSP". Boards that fit this description
 * include the Arduino Duemilanove, Arduino Diecimila, Arduino NG
 * (http://arduino.cc/it/main/boards) and the BitWizard ftdi_atmega board
 * (http://www.bitwizard.nl/wiki/index.php/FTDI_ATmega)
 *
 * The Arduinos have to be patched to bring some of the control lines to the
 * ICSP header. The BitWizard board already has the neccessary wiring on the
 * PCB.
 *
 * How to add the wires to an arduino is documented here:
 * http://www.geocities.jp/arduino_diecimila/bootloader/index_en.html
*/

#include <ac_cfg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>

#include "avrdude.h"
#include "libavrdude.h"

#include "bitbang.h"
#include "ft245r.h"
#include "usbdevs.h"

#include "tpi.h"

#define TPIPCR_GT_0b    0x07
#define TPI_STOP_BITS   0x03

#if defined(HAVE_LIBFTDI1) && defined(HAVE_LIBUSB_1_0)

#if defined(HAVE_LIBUSB_1_0_LIBUSB_H)
#include <libusb-1.0/libusb.h>
#else
#include <libusb.h>
#endif

#include <libftdi1/ftdi.h>
#elif defined(HAVE_LIBFTDI)
#include <ftdi.h>
#else

#ifdef _MSC_VER
#pragma message("No libftdi or libusb support. Install libftdi1/libusb-1.0 or libftdi/libusb and run configure/make again.")
#else
#warning No libftdi or libusb support. Install libftdi1/libusb-1.0 or libftdi/libusb and run configure/make again.
#endif

#define DO_NOT_BUILD_FT245R
#endif

#if defined(DO_NOT_BUILD_FT245R)
static int ft245r_noftdi_open(PROGRAMMER *pgm, const char *name) {
  pmsg_error("no libftdi or libusb support; install libftdi1/libusb-1.0 or libftdi/libusb and run configure/make\n");

  return -1;
}

void ft245r_initpgm(PROGRAMMER *pgm) {
  strcpy(pgm->type, "ftdi_syncbb");
  pgm->open = ft245r_noftdi_open;
}

#else

#define FT245R_CYCLES         2
#define FT245R_CMD_SIZE      (4*8*FT245R_CYCLES)
#define FT245R_FRAGMENT_SIZE (8*FT245R_CMD_SIZE)
#define REQ_OUTSTANDINGS     10

#define FT245R_DEBUG          0

/*
 * Some revisions of the FTDI chips mess up the timing in bitbang mode unless
 * the bitclock is set to the max (3 MHz).  For example, see:
 *
 * http://www.ftdichip.com/Support/Documents/TechnicalNotes/TN_120_FT232R%20Errata%20Technical%20Note.pdf
 *
 * To work around this problem, set the macro below to 1 to always set the
 * bitclock to 3 MHz and then issue the same byte repeatedly to get the desired
 * timing.
 *
*/
#define FT245R_BITBANG_VARIABLE_PULSE_WIDTH_WORKAROUND 0

// Private data for this programmer

#define FT245R_BUFSIZE       0x2000 // Receive buffer size
#define FT245R_MIN_FIFO_SIZE    128 // Min of FTDI RX/TX FIFO size

struct pdata {
  struct ftdi_context *handle;

#if FT245R_BITBANG_VARIABLE_PULSE_WIDTH_WORKAROUND
  int baud_mult;
#define baud_multiplier my.baud_mult
#else
#define baud_multiplier 1       // Optimised by C compiler
#endif

  unsigned char ft245r_ddr;
  unsigned char ft245r_out;
  struct {
    int len;                    // # of bytes in transmit buffer
    uint8_t buf[FT245R_MIN_FIFO_SIZE];  // Transmit buffer
  } tx;
  struct {
    int discard;                // # of bytes to discard during read
    int pending;                // # of bytes that have been written since last read
    int len;                    // # of bytes in receive buffer
    int wr;                     // Write pointer
    int rd;                     // Read pointer
    uint8_t buf[FT245R_BUFSIZE];        // Receive ring buffer
  } rx;
  struct ft245r_request {
    int addr;
    int bytes;
    int n;
    struct ft245r_request *next;
  } *req_head, *req_tail, *req_pool;
};

// Use private programmer data as if they were a global structure my
#define my (*(struct pdata *)(pgm->cookie))

static int ft245r_cmd(const PROGRAMMER *pgm, const unsigned char *cmd, unsigned char *res);
static int ft245r_tpi_tx(const PROGRAMMER *pgm, uint8_t byte);
static int ft245r_tpi_rx(const PROGRAMMER *pgm, uint8_t *bytep);

// Discard all data from the receive buffer.
static void ft245r_rx_buf_purge(const PROGRAMMER *pgm) {
  my.rx.len = 0;
  my.rx.rd = my.rx.wr = 0;
}

static void ft245r_rx_buf_put(const PROGRAMMER *pgm, uint8_t byte) {
  my.rx.len++;
  my.rx.buf[my.rx.wr++] = byte;
  if(my.rx.wr >= (int) sizeof(my.rx.buf))
    my.rx.wr = 0;
}

static uint8_t ft245r_rx_buf_get(const PROGRAMMER *pgm) {
  my.rx.len--;
  uint8_t byte = my.rx.buf[my.rx.rd++];

  if(my.rx.rd >= (int) sizeof(my.rx.buf))
    my.rx.rd = 0;
  return byte;
}

// Fill receive buffer with data from the FTDI receive FIFO
static int ft245r_fill(const PROGRAMMER *pgm) {
  uint8_t raw[FT245R_MIN_FIFO_SIZE];
  int i, nread;

  nread = ftdi_read_data(my.handle, raw, my.rx.pending);
  if(nread < 0)
    return -1;
  my.rx.pending -= nread;

#if FT245R_DEBUG
  msg_info("%s: read %d bytes (pending=%d)\n", __func__, nread, my.rx.pending);
#endif

  for(i = 0; i < nread; ++i)
    ft245r_rx_buf_put(pgm, raw[i]);
  return nread;
}

static int ft245r_rx_buf_fill_and_get(const PROGRAMMER *pgm) {
  while(my.rx.len == 0) {
    int result = ft245r_fill(pgm);

    if(result < 0)
      return result;
  }

  return ft245r_rx_buf_get(pgm);
}

// Flush pending TX data to the FTDI send FIFO
static int ft245r_flush(const PROGRAMMER *pgm) {
  int rv, len = my.tx.len, avail;
  uint8_t *src = my.tx.buf;

  if(!len)
    return 0;

  while(len > 0) {
    avail = FT245R_MIN_FIFO_SIZE - my.rx.pending;
    if(avail <= 0) {
      avail = ft245r_fill(pgm);
      if(avail < 0) {
        pmsg_error("fill returned %d: %s\n", avail, ftdi_get_error_string(my.handle));
        return -1;
      }
    }
    if(avail > len)
      avail = len;

#if FT245R_DEBUG
    msg_notice("%s: writing %d bytes\n", __func__, avail);
#endif

    rv = ftdi_write_data(my.handle, src, avail);
    if(rv != avail) {
      msg_error("write returned %d (expected %d): %s\n", rv, avail, ftdi_get_error_string(my.handle));
      return -1;
    }
    src += avail;
    len -= avail;
    my.rx.pending += avail;
  }

  return 0;
}

static int ft245r_send2(const PROGRAMMER *pgm, unsigned char *buf, size_t len, bool discard_rx_data) {
  for(size_t i = 0; i < len; ++i) {
    for(int j = 0; j < baud_multiplier; ++j) {
      if(discard_rx_data)
        ++my.rx.discard;
      my.tx.buf[my.tx.len++] = buf[i];
      if(my.tx.len >= FT245R_MIN_FIFO_SIZE) {
        ft245r_flush(pgm);
        my.tx.len = 0;
      }
    }
  }
  return 0;
}

static int ft245r_send(const PROGRAMMER *pgm, unsigned char *buf, size_t len) {
  return ft245r_send2(pgm, buf, len, false);
}

static int ft245r_send_and_discard(const PROGRAMMER *pgm, unsigned char *buf, size_t len) {
  return ft245r_send2(pgm, buf, len, true);
}

static int ft245r_recv(const PROGRAMMER *pgm, unsigned char *buf, size_t len) {
  ft245r_flush(pgm);
  my.tx.len = 0;
  ft245r_fill(pgm);

#if FT245R_DEBUG
  msg_notice("%s: discarding %d, consuming %lu bytes\n", __func__, my.rx.discard, (unsigned long) len);
#endif

  while(my.rx.discard > 0) {
    int result = ft245r_rx_buf_fill_and_get(pgm);

    if(result < 0)
      return result;

    --my.rx.discard;
  }

  for(size_t i = 0; i < len; ++i) {
    int result = ft245r_rx_buf_fill_and_get(pgm);

    if(result < 0)
      return result;

    buf[i] = (uint8_t) result;
#if FT245R_BITBANG_VARIABLE_PULSE_WIDTH_WORKAROUND
    for(int j = 1; j < baud_multiplier; ++j) {
      result = ft245r_rx_buf_fill_and_get(pgm);
      if(result < 0)
        return result;
    }
#endif
  }
  return 0;
}

static int ft245r_drain(const PROGRAMMER *pgm, int display) {
  int r;

  // Flush the buffer in the chip by changing the mode ...
  r = ftdi_set_bitmode(my.handle, 0, BITMODE_RESET); // Reset
  if(r)
    return -1;
  r = ftdi_set_bitmode(my.handle, my.ft245r_ddr, BITMODE_SYNCBB); // Set synchronous bitbang
  if(r)
    return -1;

  // Drain our buffer
  ft245r_rx_buf_purge(pgm);
  return 0;
}

// Ensure any pending writes are sent to the FTDI chip before sleeping
static void ft245r_usleep(const PROGRAMMER *pgm, useconds_t usec) {
  ft245r_flush(pgm);
  my.tx.len = 0;
  usleep(usec);
}

static int ft245r_chip_erase(const PROGRAMMER *pgm, const AVRPART *p) {
  unsigned char cmd[4] = { 0, 0, 0, 0 };
  unsigned char res[4];

  if(is_tpi(p))
    return avr_tpi_chip_erase(pgm, p);

  if(p->op[AVR_OP_CHIP_ERASE] == NULL) {
    msg_error("chip erase instruction not defined for part %s\n", p->desc);
    return -1;
  }

  avr_set_bits(p->op[AVR_OP_CHIP_ERASE], cmd);
  pgm->cmd(pgm, cmd, res);
  ft245r_usleep(pgm, p->chip_erase_delay);
  return pgm->initialize(pgm, p);
}

static int ft245r_set_bitclock(const PROGRAMMER *pgm) {
  // libftdi1 multiplies bitbang baudrate by 4
  int r, rate = 0, ftdi_rate = 3000000/4; // Max rate for ft232r is 750000

  if(pgm->baudrate || pgm->bitclock)
    if(!(pgm->extra_features & HAS_BITCLOCK_ADJ))
      pmsg_warning("setting bitclock despite HAS_BITCLOCK_ADJ missing in pgm->extra_features\n");

  if(pgm->baudrate && pgm->bitclock && (int) (1.0/pgm->bitclock) != pgm->baudrate)
    pmsg_warning("both -b baudrate and -B bitrate set; using -B\n");

  // 150000 should work for all ftdi chips and the internal clock of 1 MHz
  rate = pgm->bitclock? (int) (1.0/pgm->bitclock): pgm->baudrate? pgm->baudrate: 150000;

#if FT245R_BITBANG_VARIABLE_PULSE_WIDTH_WORKAROUND
  my.baud_mult = rate > 0 && rate < ftdi_rate? round((ftdi_rate + rate - 1)/rate): 1;
#else
  ftdi_rate = rate;
#endif

  msg_notice2("%s: bitclk %d -> FTDI rate %d, baud multiplier %d\n", __func__, rate, ftdi_rate, baud_multiplier);

  r = ftdi_set_baudrate(my.handle, ftdi_rate);
  if(r) {
    msg_error("setting baudrate %d failed with error %s\n", rate, ftdi_get_error_string(my.handle));
    return -1;
  }
  return 0;
}

static int get_pin(const PROGRAMMER *pgm, int pinname) {
  uint8_t byte;

  ft245r_flush(pgm);
  my.tx.len = 0;

  if(ftdi_read_pins(my.handle, &byte) != 0)
    return -1;
  if(FT245R_DEBUG)
    msg_notice("%s: in 0x%02x\n", __func__, byte);
  return GET_BITS_0(byte, pgm, pinname) != 0;
}

static int set_pin(const PROGRAMMER *pgm, int pinname, int val) {
  unsigned char buf[1];

  if(pgm->pin[pinname].mask[0] == 0) {
    // Ignore not defined pins (might be the led or vcc or buff if not needed)
    return 0;
  }

  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, pinname, val);
  buf[0] = my.ft245r_out;

  ft245r_send_and_discard(pgm, buf, 1);
  return 0;
}

static int set_sck(const PROGRAMMER *pgm, int value) {
  return set_pin(pgm, PIN_AVR_SCK, value);
}

static int set_reset(const PROGRAMMER *pgm, int value) {
  return set_pin(pgm, PIN_AVR_RESET, value);
}

static int set_buff(const PROGRAMMER *pgm, int value) {
  return set_pin(pgm, PPI_AVR_BUFF, value);
}

static int set_vcc(const PROGRAMMER *pgm, int value) {
  return set_pin(pgm, PPI_AVR_VCC, value);
}

/* these functions are callbacks, which go into the
 * PROGRAMMER data structure ("optional functions")
 */
static int ft245_rdy_led(const PROGRAMMER *pgm, int value) {
  return set_pin(pgm, PIN_LED_RDY, value);
}

static int ft245_err_led(const PROGRAMMER *pgm, int value) {
  return set_pin(pgm, PIN_LED_ERR, value);
}

static int ft245_pgm_led(const PROGRAMMER *pgm, int value) {
  return set_pin(pgm, PIN_LED_PGM, value);
}

static int ft245_vfy_led(const PROGRAMMER *pgm, int value) {
  return set_pin(pgm, PIN_LED_VFY, value);
}

// Apply power to the AVR processor
static void ft245r_powerup(const PROGRAMMER *pgm) {
  set_vcc(pgm, ON);             // Power up
  ft245r_usleep(pgm, 100);
}

// Remove power from the AVR processor
static void ft245r_powerdown(const PROGRAMMER *pgm) {
  set_vcc(pgm, OFF);            // Power down
}

static void ft245r_disable(const PROGRAMMER *pgm) {
  set_buff(pgm, OFF);
}

static void ft245r_enable(PROGRAMMER *pgm, const AVRPART *p) {
  /*
   * Prepare to start talking to the connected device - pull reset low first,
   * delay a few milliseconds, then enable the buffer.  This sequence allows
   * the AVR to be reset before the buffer is enabled to avoid a short period
   * of time where the AVR may be driving the programming lines at the same
   * time the programmer tries to.  Of course, if a buffer is being used, then
   * the /RESET line from the programmer needs to be directly connected to the
   * AVR /RESET line and not via the buffer chip.
   */
  set_reset(pgm, OFF);
  ft245r_usleep(pgm, 1);
  set_buff(pgm, ON);
}

// Issue the 'program enable' command to the AVR device
static int ft245r_program_enable(const PROGRAMMER *pgm, const AVRPART *p) {
  unsigned char cmd[4] = { 0, 0, 0, 0 };
  unsigned char res[4];
  int i;

  if(is_tpi(p))
    return avr_tpi_program_enable(pgm, p, TPIPCR_GT_0b);

  if(p->op[AVR_OP_PGM_ENABLE] == NULL) {
    pmsg_error("AVR_OP_PGM_ENABLE command not defined for %s\n", p->desc);
    fflush(stderr);
    return -1;
  }

  avr_set_bits(p->op[AVR_OP_PGM_ENABLE], cmd);

  for(i = 0; i < 4; i++) {
    ft245r_cmd(pgm, cmd, res);

    if(res[p->pollindex - 1] == p->pollvalue)
      return 0;

    if(FT245R_DEBUG) {
      pmsg_notice("program enable command not successful, retrying\n");
      fflush(stderr);
    }
    set_pin(pgm, PIN_AVR_RESET, ON);
    ft245r_usleep(pgm, 20);
    set_pin(pgm, PIN_AVR_RESET, OFF);

    if(i == 3) {
      ft245r_drain(pgm, 0);
      ft245r_rx_buf_purge(pgm);
    }
  }

  pmsg_error("device is not responding to program enable; check connection\n");
  fflush(stderr);

  return -1;
}

// Initialize the AVR device and prepare it to accept commands
static int ft245r_initialize(const PROGRAMMER *pgm, const AVRPART *p) {

  /* Apply power between VCC and GND while RESET and SCK are set to “0”. In some systems,
   * the programmer can not guarantee that SCK is held low during power-up. In this
   * case, RESET must be given a positive pulse of at least two CPU clock cycles duration
   * after SCK has been set to “0”.
   */
  set_sck(pgm, OFF);
  ft245r_powerup(pgm);

  set_reset(pgm, OFF);
  ft245r_usleep(pgm, 5000);     // 5ms
  set_reset(pgm, ON);
  ft245r_usleep(pgm, 5000);     // 5ms
  set_reset(pgm, OFF);

  /* Wait for at least 20 ms and enable serial programming by sending the Programming
   * Enable serial instruction to pin SDO.
   */
  ft245r_usleep(pgm, 20000);    // 20ms

  if(is_tpi(p)) {
    bool io_link_ok = true;
    uint8_t byte;
    int i;

    /*
     * Since there is a single TPIDATA line, SDO and SDI must be linked
     * together through a 1kOhm resistor.  Verify that everything we send on
     * SDO gets mirrored back on SDI.
     */
    set_pin(pgm, PIN_AVR_SDO, 0);
    if(get_pin(pgm, PIN_AVR_SDI) != 0) {
      io_link_ok = false;
      if(ovsigck) {
        pmsg_warning("SDO->SDI 0 failed\n");
      } else {
        pmsg_error("SDO->SDI 0 failed\n");
        return -1;
      }
    }
    set_pin(pgm, PIN_AVR_SDO, 1);
    if(get_pin(pgm, PIN_AVR_SDI) != 1) {
      io_link_ok = false;
      if(ovsigck) {
        pmsg_warning("SDO->SDI 1 failed\n");
      } else {
        pmsg_error("SDO->SDI 1 failed\n");
        return -1;
      }
    }

    if(io_link_ok)
      msg_notice2("SDO-SDI link present\n");

    // Keep TPIDATA high for 16 clock cycles
    set_pin(pgm, PIN_AVR_SDO, 1);
    for(i = 0; i < 16; i++) {
      set_sck(pgm, 1);
      set_sck(pgm, 0);
    }

    // Remove extra guard timing bits
    ft245r_tpi_tx(pgm, TPI_CMD_SSTCS | TPI_REG_TPIPCR);
    ft245r_tpi_tx(pgm, 0x7);

    // Read TPI ident reg
    ft245r_tpi_tx(pgm, TPI_CMD_SLDCS | TPI_REG_TPIIR);
    ft245r_tpi_rx(pgm, &byte);
    if(byte != 0x80) {
      msg_error("TPIIR 0x%02x not correct\n", byte);
      return -1;
    }
  }
  return ft245r_program_enable(pgm, p);
}

static inline void add_bit(const PROGRAMMER *pgm, unsigned char *buf, int *buf_pos, uint8_t bit) {
  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PIN_AVR_SDO, bit);
  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PIN_AVR_SCK, 0);
  buf[*buf_pos] = my.ft245r_out;
  (*buf_pos)++;

  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PIN_AVR_SCK, 1);
  buf[*buf_pos] = my.ft245r_out;
  (*buf_pos)++;
}

static inline int set_data(const PROGRAMMER *pgm, unsigned char *buf, unsigned char data) {
  int j;
  int buf_pos = 0;
  unsigned char bit = 0x80;

  for(j = 0; j < 8; j++) {
    add_bit(pgm, buf, &buf_pos, (data & bit) != 0);
    bit >>= 1;
  }
  return buf_pos;
}

static inline unsigned char extract_data(const PROGRAMMER *pgm, unsigned char *buf, int offset) {
  int j;
  int buf_pos = FT245R_CYCLES;  /* SDI data is valid AFTER rising SCK edge,
                                   i.e. in next clock cycle */
  unsigned char bit = 0x80;
  unsigned char r = 0;

  buf += offset*(8*FT245R_CYCLES);
  for(j = 0; j < 8; j++) {
    if(GET_BITS_0(buf[buf_pos], pgm, PIN_AVR_SDI))
      r |= bit;
    buf_pos += FT245R_CYCLES;
    bit >>= 1;
  }
  return r;
}

// To check data

#if 0
static inline unsigned char extract_data_out(const PROGRAMMER *pgm, unsigned char *buf, int offset) {
  int j;
  int buf_pos = 1;
  unsigned char bit = 0x80;
  unsigned char r = 0;

  buf += offset*(8*FT245R_CYCLES);
  for(j = 0; j < 8; j++) {
    if(GET_BITS_0(buf[buf_pos], pgm, PIN_AVR_SDO))
      r |= bit;
    buf_pos += FT245R_CYCLES;
    bit >>= 1;
  }
  return r;
}
#endif

/*
 * Transmit an AVR device command and return the results; 'cmd' and 'res' must
 * point to at least a 4 byte data buffer
 */
static int ft245r_cmd(const PROGRAMMER *pgm, const unsigned char *cmd, unsigned char *res) {
  int i, buf_pos;
  unsigned char buf[128];

  buf_pos = 0;
  for(i = 0; i < 4; i++)
    buf_pos += set_data(pgm, buf + buf_pos, cmd[i]);
  buf[buf_pos] = 0;
  buf_pos++;

  ft245r_send(pgm, buf, buf_pos);
  ft245r_recv(pgm, buf, buf_pos);
  res[0] = extract_data(pgm, buf, 0);
  res[1] = extract_data(pgm, buf, 1);
  res[2] = extract_data(pgm, buf, 2);
  res[3] = extract_data(pgm, buf, 3);

  return 0;
}

static inline uint8_t extract_tpi_data(const PROGRAMMER *pgm, unsigned char *buf, int *buf_pos) {
  uint8_t bit = 0x1, byte = 0;
  int j;

  for(j = 0; j < 8; j++) {
    (*buf_pos)++;               // Skip over falling clock edge
    if(GET_BITS_0(buf[(*buf_pos)++], pgm, PIN_AVR_SDI))
      byte |= bit;
    bit <<= 1;
  }
  return byte;
}

static inline int set_tpi_data(const PROGRAMMER *pgm, unsigned char *buf, uint8_t byte) {
  uint8_t bit = 0x1, parity = 0;
  int j, buf_pos = 0;

  // Start bit
  add_bit(pgm, buf, &buf_pos, 0);

  // 8 data bits:
  for(j = 0; j < 8; j++) {
    add_bit(pgm, buf, &buf_pos, (byte & bit) != 0);
    parity ^= (byte & bit) != 0;
    bit <<= 1;
  }

  // Parity bit
  add_bit(pgm, buf, &buf_pos, parity);
  // Stop bits
  add_bit(pgm, buf, &buf_pos, 1);
  add_bit(pgm, buf, &buf_pos, 1);
  return buf_pos;
}

static int ft245r_tpi_tx(const PROGRAMMER *pgm, uint8_t byte) {
  uint8_t buf[128];
  int len;

  len = set_tpi_data(pgm, buf, byte);
  ft245r_send_and_discard(pgm, buf, len);
  return 0;
}

static int ft245r_tpi_rx(const PROGRAMMER *pgm, uint8_t *bytep) {
  uint8_t buf[128], bit, parity;
  int i, buf_pos = 0, len = 0;
  uint32_t res, m, byte;

  // Allow for up to 4 bits before we must see start bit; during that time, we must keep the SDO line high
  for(i = 0; i < 2; ++i)
    len += set_data(pgm, &buf[len], 0xff);

  ft245r_send(pgm, buf, len);
  ft245r_recv(pgm, buf, len);

  res = (extract_tpi_data(pgm, buf, &buf_pos)
    | ((uint32_t) extract_tpi_data(pgm, buf, &buf_pos) << 8));

  // Look for start bit
  m = 0x1;
  while(m & res)
    m <<= 1;
  if(m >= 0x10) {
    pmsg_error("start bit missing (res=0x%04x)\n", res);
    return -1;
  }
  byte = parity = 0;
  for(i = 0; i < 8; ++i) {
    m <<= 1;
    bit = (res & m) != 0;
    parity ^= bit;
    byte |= bit << i;
  }
  m <<= 1;
  if(((res & m) != 0) != parity) {
    pmsg_error("parity bit wrong\n");
    return -1;
  }
  if(((res & (m << 1)) == 0) || ((res & (m << 2))) == 0) {
    pmsg_error("stop bits wrong\n");
    return -1;
  }
  *bytep = (uint8_t) byte;
  return 0;
}

static int ft245r_cmd_tpi(const PROGRAMMER *pgm, const unsigned char *cmd,
  int cmd_len, unsigned char *res, int res_len) {

  int i, ret = 0;

  for(i = 0; i < cmd_len; ++i)
    ft245r_tpi_tx(pgm, cmd[i]);
  for(i = 0; i < res_len; ++i)
    if((ret = ft245r_tpi_rx(pgm, &res[i])) < 0)
      break;
  if(verbose >= MSG_DEBUG) {
    msg_debug("%s: [ ", __func__);
    for(i = 0; i < cmd_len; i++)
      msg_debug("%02X ", cmd[i]);
    msg_debug("] [ ");
    for(i = 0; i < res_len; i++)
      msg_debug("%02X ", res[i]);
    msg_debug("]\n");
  }

  return ret;
}

// Lower 8 pins are accepted, they might be also inverted
static const struct pindef valid_pins = { {0xff}, {0xff} };

static const Pin_checklist pin_checklist[] = {
  {PIN_AVR_SCK, 1, &valid_pins},
  {PIN_AVR_SDO, 1, &valid_pins},
  {PIN_AVR_SDI, 1, &valid_pins},
  {PIN_AVR_RESET, 1, &valid_pins},
  {PPI_AVR_BUFF, 0, &valid_pins},
};

static int ft245r_open(PROGRAMMER *pgm, const char *port) {
  int rv;
  int devnum = -1;
  char device[9] = "";

  rv = pins_check(pgm, pin_checklist, sizeof(pin_checklist)/sizeof(pin_checklist[0]), true);

  if(rv) {
    pgm->display(pgm, "");
    return rv;
  }

  pgm->port = port;

  // Read device string cut after 8 chars (max. length of serial number)
  if((sscanf(port, "usb:%8s", device) != 1)) {
    pmsg_notice("%s(): no device identifier in portname, using default\n", __func__);
    pgm->usbsn = cache_string("");
    devnum = 0;
  } else {
    if(strlen(device) == 8) {   // Serial number
      pmsg_notice2("%s(): serial number parsed as: %s\n", __func__, device);
      // Copy serial number to pgm struct
      pgm->usbsn = cache_string(device);
      // and use first device with matching serial (should be unique)
      devnum = 0;
    } else if(strncmp("ft", device, 2) || strlen(device) <= 8) {        // Classic device number
      char *startptr = device + 2;
      char *endptr = NULL;

      devnum = strtol(startptr, &endptr, 10);
      if((startptr == endptr) || (*endptr != '\0'))
        devnum = -1;
      pmsg_notice2("%s(): device number parsed as: %d\n", __func__, devnum);
    }
  }

  // If something went wrong before abort with helpful message
  if(devnum < 0) {
    pmsg_error("invalid port name %s: use ft[0-9]+ or serial number\n", port);
    return -1;
  }

  my.handle = mmt_malloc(sizeof(struct ftdi_context));
  ftdi_init(my.handle);
  LNODEID usbpid = lfirst(pgm->usbpid);
  int pid;

  if(usbpid) {
    pid = *(int *) (ldata(usbpid));
    if(lnext(usbpid))
      pmsg_warning("using PID 0x%04x, ignoring remaining PIDs in list\n", pid);
  } else {
    pid = USB_DEVICE_FT245;
  }
  rv = ftdi_usb_open_desc_index(my.handle,
    pgm->usbvid? pgm->usbvid: USB_VENDOR_FTDI,
    pid, pgm->usbproduct[0]? pgm->usbproduct: NULL, pgm->usbsn[0]? pgm->usbsn: NULL, devnum);
  if(rv) {
    pmsg_error("cannot open ftdi device: %s\n", ftdi_get_error_string(my.handle));
    goto cleanup_no_usb;
  }

  my.ft245r_ddr = pgm->pin[PIN_AVR_SCK].mask[0]
    | pgm->pin[PIN_AVR_SDO].mask[0]
    | pgm->pin[PIN_AVR_RESET].mask[0]
    | pgm->pin[PPI_AVR_BUFF].mask[0]
    | pgm->pin[PPI_AVR_VCC].mask[0]
    | pgm->pin[PIN_LED_ERR].mask[0]
    | pgm->pin[PIN_LED_RDY].mask[0]
    | pgm->pin[PIN_LED_PGM].mask[0]
    | pgm->pin[PIN_LED_VFY].mask[0];

  // Set initial values for outputs, no reset everything else is off
  my.ft245r_out = 0;
  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PIN_AVR_RESET, 1);
  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PIN_AVR_SCK, 0);
  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PIN_AVR_SDO, 0);
  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PPI_AVR_BUFF, 0);
  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PPI_AVR_VCC, 0);
  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PIN_LED_ERR, 0);
  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PIN_LED_RDY, 0);
  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PIN_LED_PGM, 0);
  my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PIN_LED_VFY, 0);

  rv = ftdi_set_latency_timer(my.handle, 1);
  if(rv) {
    pmsg_error("unable to set latency timer to 1 (%s)\n", ftdi_get_error_string(my.handle));
    goto cleanup;
  }

  rv = ftdi_set_bitmode(my.handle, my.ft245r_ddr, BITMODE_SYNCBB); // Set Synchronous bitbang
  if(rv) {
    pmsg_error("synchronous BitBangMode is not supported (%s)\n", ftdi_get_error_string(my.handle));
    goto cleanup;
  }

  rv = ft245r_set_bitclock(pgm);
  if(rv)
    goto cleanup;

  // Drain any extraneous input
  ft245r_drain(pgm, 0);

  ft245r_send_and_discard(pgm, &my.ft245r_out, 1);

  return 0;

cleanup:
  ftdi_usb_close(my.handle);
cleanup_no_usb:
  ftdi_deinit(my.handle);
  mmt_free(my.handle);
  my.handle = NULL;
  return -1;
}

static void ft245r_close(PROGRAMMER *pgm) {
  if(my.handle) {
    // I think the switch to BB mode and back flushes the buffer.
    ftdi_set_bitmode(my.handle, 0, BITMODE_SYNCBB); // Set Synchronous BitBang, all in puts
    ftdi_set_bitmode(my.handle, 0, BITMODE_RESET);  // Disable Synchronous BitBang
    ftdi_usb_close(my.handle);
    ftdi_deinit(my.handle);
    mmt_free(my.handle);
    my.handle = NULL;
  }
}

static void ft245r_display(const PROGRAMMER *pgm, const char *p) {
  msg_info("%sPin assignment        : 0..7 = DBUS0..7\n", p);   // , 8..11 = GPIO0..3\n", p);
  pgm_display_generic_mask(pgm, p, SHOW_ALL_PINS);
}

static int ft245r_paged_write_gen(const PROGRAMMER *pgm, const AVRPART *p, const AVRMEM *m,
  unsigned int page_size, unsigned int addr, unsigned int n_bytes) {

  for(int i = 0; i < (int) n_bytes; i++, addr++)
    if(avr_write_byte_default(pgm, p, m, addr, m->buf[addr]) != 0)
      return -2;

  return n_bytes;
}

static void put_request(const PROGRAMMER *pgm, int addr, int bytes, int n) {
  struct ft245r_request *p;

  if(my.req_pool) {             // Allocate struct ft245r_request from pool
    p = my.req_pool;
    my.req_pool = p->next;
  } else {                      // ... or from memory
    p = mmt_malloc(sizeof(struct ft245r_request));
  }
  memset(p, 0, sizeof(struct ft245r_request));
  p->addr = addr;
  p->bytes = bytes;
  p->n = n;
  if(my.req_tail) {
    my.req_tail->next = p;
    my.req_tail = p;
  } else {
    my.req_head = my.req_tail = p;
  }
}

static int do_request(const PROGRAMMER *pgm, const AVRMEM *m) {
  struct ft245r_request *p;
  int addr, bytes, j, n;
  unsigned char buf[FT245R_FRAGMENT_SIZE + 1 + 128];

  if(!my.req_head)
    return 0;
  p = my.req_head;
  my.req_head = p->next;
  if(!my.req_head)
    my.req_tail = my.req_head;

  addr = p->addr;
  bytes = p->bytes;
  n = p->n;
  // Insert p into request pool
  memset(p, 0, sizeof(struct ft245r_request));
  p->next = my.req_pool;
  my.req_pool = p;

  ft245r_recv(pgm, buf, bytes);
  for(j = 0; j < n; j++)
    m->buf[addr++] = extract_data(pgm, buf, (j*4 + 3));
  return 1;
}

static int ft245r_paged_write_flash(const PROGRAMMER *pgm, const AVRPART *p, const AVRMEM *m,
  unsigned int page_size, unsigned int addr, unsigned int n_bytes) {

  int i, j, addr_save, buf_pos, req_count, do_page_write;
  unsigned char buf[FT245R_FRAGMENT_SIZE + 1];
  unsigned char cmd[4];

  if(m->op[AVR_OP_LOADPAGE_LO] == NULL || m->op[AVR_OP_LOADPAGE_HI] == NULL) {
    msg_error("AVR_OP_LOADPAGE_HI/LO command not defined for %s\n", p->desc);
    return -1;
  }

  do_page_write = req_count = i = j = buf_pos = 0;
  addr_save = addr;
  while(i < (int) n_bytes) {
    int spi = addr & 1? AVR_OP_LOADPAGE_HI: AVR_OP_LOADPAGE_LO;

    // Put the SPI loadpage command as FT245R_CMD_SIZE bytes into buffer
    memset(cmd, 0, sizeof cmd);
    avr_set_bits(m->op[spi], cmd);
    avr_set_addr(m->op[spi], cmd, addr/2);
    avr_set_input(m->op[spi], cmd, m->buf[addr]);
    for(size_t k = 0; k < sizeof cmd; k++)
      buf_pos += set_data(pgm, buf + buf_pos, cmd[k]);

    i++;
    j++;
    addr++;

    if(m->paged && (i%m->page_size == 0 || i >= (int) n_bytes))
      do_page_write = 1;

    // Page boundary, finished or buffer exhausted? queue up requests
    if(do_page_write || i >= (int) n_bytes || j >= FT245R_FRAGMENT_SIZE/FT245R_CMD_SIZE) {
      if(i >= (int) n_bytes) {
        my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PIN_AVR_SCK, 0); // SCK down
        buf[buf_pos++] = my.ft245r_out;
      } else {
        // Stretch sequence to allow correct readout, see extract_data()
        buf[buf_pos] = buf[buf_pos - 1];
        buf_pos++;
      }
      ft245r_send(pgm, buf, buf_pos);
      put_request(pgm, addr_save, buf_pos, 0);

      if(++req_count > REQ_OUTSTANDINGS)
        do_request(pgm, m);

      if(do_page_write) {
        while(do_request(pgm, m))
          continue;
        if(avr_write_page(pgm, p, m, addr_save - (addr_save%m->page_size)) != 0)
          return -2;
        do_page_write = req_count = 0;
      }

      // Reset buffer variables
      j = buf_pos = 0;
      addr_save = addr;
    }
  }

  while(do_request(pgm, m))
    continue;

  return n_bytes;
}

static int ft245r_paged_write(const PROGRAMMER *pgm, const AVRPART *p, const AVRMEM *m,
  unsigned int page_size, unsigned int addr, unsigned int n_bytes) {

  if(!n_bytes)
    return 0;

  if(mem_is_flash(m))
    return ft245r_paged_write_flash(pgm, p, m, page_size, addr, n_bytes);

  if(mem_is_eeprom(m))
    return ft245r_paged_write_gen(pgm, p, m, page_size, addr, n_bytes);

  return -2;
}

static int ft245r_paged_load_gen(const PROGRAMMER *pgm, const AVRPART *p, const AVRMEM *m,
  unsigned int page_size, unsigned int addr, unsigned int n_bytes) {

  for(int i = 0; i < (int) n_bytes; i++) {
    unsigned char rbyte;

    if(avr_read_byte_default(pgm, p, m, addr + i, &rbyte) != 0)
      return -2;

    m->buf[addr + i] = rbyte;
  }

  return 0;
}

static int ft245r_paged_load_flash(const PROGRAMMER *pgm, const AVRPART *p, const AVRMEM *m,
  unsigned int page_size, unsigned int addr, unsigned int n_bytes) {

  int i, j, addr_save, buf_pos, req_count;
  unsigned char buf[FT245R_FRAGMENT_SIZE + 1];
  unsigned char cmd[4];

  if(m->op[AVR_OP_READ_LO] == NULL || m->op[AVR_OP_READ_HI] == NULL) {
    msg_error("AVR_OP_READ_HI/LO command not defined for %s\n", p->desc);
    return -1;
  }

  /*
   * Always called with addr at page boundary, and n_bytes == m->page_size;
   * hence, OK to prepend load extended address command (at most) once
   */
  if(m->op[AVR_OP_LOAD_EXT_ADDR]) {
    memset(cmd, 0, sizeof cmd);
    avr_set_bits(m->op[AVR_OP_LOAD_EXT_ADDR], cmd);
    avr_set_addr(m->op[AVR_OP_LOAD_EXT_ADDR], cmd, addr/2);

    buf_pos = 0;
    for(size_t k = 0; k < sizeof cmd; k++)
      buf_pos += set_data(pgm, buf + buf_pos, cmd[k]);
    ft245r_send_and_discard(pgm, buf, buf_pos);
  }

  req_count = i = j = buf_pos = 0;
  addr_save = addr;
  while(i < (int) n_bytes) {
    int spi = addr & 1? AVR_OP_READ_HI: AVR_OP_READ_LO;

    // Put the SPI read command as FT245R_CMD_SIZE bytes into buffer
    memset(cmd, 0, sizeof cmd);
    avr_set_bits(m->op[spi], cmd);
    avr_set_addr(m->op[spi], cmd, addr/2);
    for(size_t k = 0; k < sizeof cmd; k++)
      buf_pos += set_data(pgm, buf + buf_pos, cmd[k]);

    i++;
    j++;
    addr++;

    // Finished or buffer exhausted? Queue up requests
    if(i >= (int) n_bytes || j >= FT245R_FRAGMENT_SIZE/FT245R_CMD_SIZE) {
      if(i >= (int) n_bytes) {
        my.ft245r_out = SET_BITS_0(my.ft245r_out, pgm, PIN_AVR_SCK, 0); // SCK down
        buf[buf_pos++] = my.ft245r_out;
      } else {
        // Stretch sequence to allow correct readout, see extract_data()
        buf[buf_pos] = buf[buf_pos - 1];
        buf_pos++;
      }
      ft245r_send(pgm, buf, buf_pos);
      put_request(pgm, addr_save, buf_pos, j);

      if(++req_count > REQ_OUTSTANDINGS)
        do_request(pgm, m);

      // Reset buffer variables
      j = buf_pos = 0;
      addr_save = addr;
    }
  }

  while(do_request(pgm, m))
    continue;

  return 0;
}

static int ft245r_paged_load(const PROGRAMMER *pgm, const AVRPART *p, const AVRMEM *m,
  unsigned int page_size, unsigned int addr, unsigned int n_bytes) {

  if(!n_bytes)
    return 0;

  if(mem_is_flash(m))
    return ft245r_paged_load_flash(pgm, p, m, page_size, addr, n_bytes);

  if(mem_is_eeprom(m))
    return ft245r_paged_load_gen(pgm, p, m, page_size, addr, n_bytes);

  return -2;
}

void ft245r_setup(PROGRAMMER *pgm) {
  pgm->cookie = mmt_malloc(sizeof(struct pdata));
}

void ft245r_teardown(PROGRAMMER *pgm) {
  if(!pgm->cookie)
    return;

  while(my.req_pool) {          // Free request pool
    struct ft245r_request *p = my.req_pool;

    my.req_pool = p->next;
    mmt_free(p);
  }
  while(my.req_head) {          // Free pending queue
    struct ft245r_request *p = my.req_head;

    my.req_head = p->next;
    mmt_free(p);
  }
  mmt_free(pgm->cookie);
  pgm->cookie = NULL;
}

void ft245r_initpgm(PROGRAMMER *pgm) {
  strcpy(pgm->type, "ftdi_syncbb");

  // Mandatory functions
  pgm->initialize = ft245r_initialize;
  pgm->display = ft245r_display;
  pgm->enable = ft245r_enable;
  pgm->disable = ft245r_disable;
  pgm->program_enable = ft245r_program_enable;
  pgm->chip_erase = ft245r_chip_erase;
  pgm->cmd = ft245r_cmd;
  pgm->cmd_tpi = ft245r_cmd_tpi;
  pgm->open = ft245r_open;
  pgm->close = ft245r_close;
  pgm->read_byte = avr_read_byte_default;
  pgm->write_byte = avr_write_byte_default;

  // Optional functions
  pgm->paged_write = ft245r_paged_write;
  pgm->paged_load = ft245r_paged_load;

  pgm->rdy_led = ft245_rdy_led;
  pgm->err_led = ft245_err_led;
  pgm->pgm_led = ft245_pgm_led;
  pgm->vfy_led = ft245_vfy_led;
  pgm->powerup = ft245r_powerup;
  pgm->powerdown = ft245r_powerdown;
  pgm->setup = ft245r_setup;
  pgm->teardown = ft245r_teardown;
}
#endif

const char ft245r_desc[] = "FT245R/FT232R synchronous bit-bang programmer";
