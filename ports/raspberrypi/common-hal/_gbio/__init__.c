// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// game boy cartridge interface for the PyGameBoy RP2350.
//
// The RP2350 plays the role of a game boy cartridge: it feeds a stream of SM83
// opcodes to the game boy CPU one byte at a time, synchronised to the game boy's
// own read strobes.  Two PIO state machines do the real-time work so the ARM
// core is free to run Python / USB between game boy accesses:
//
//   * gbio_ctrl  (GPIO base 0)  watches /RD and A15 and, on every cartridge ROM
//     read (A15 low, /RD low), pulses an internal handshake pin (GPIO22).
//   * gbio_data  (GPIO base 16) drives D0..D7 from its TX FIFO.  A DMA channel
//     keeps the TX FIFO full from a command buffer in RAM.  Crucially the data
//     SM drives each byte onto D0..D7 BEFORE the matching /RD strobe (mirroring
//     the SAMD port, whose port-DMA latches the output register at the END of
//     the previous read so it is stable throughout the next).  When the FIFO
//     runs dry the state machine stalls on `pull` and the pins hold the last
//     byte -- this is the "idle" state in which the game boy spins on JP (HL).
//
// Two PIO instances are required because the PyGameBoy routes D7 to GPIO36
// (only reachable with GPIO base 16) and /RD to GPIO4 (only reachable with
// GPIO base 0); a single PIO state machine cannot span both banks.  The
// handshake is a physical pin (GPIO22) which lies in the 16..31 overlap of the
// two banks, so it is visible to both state machines regardless of which PIO
// instance each one ends up on.
//
// The SM83 command streams and the address-dispatch logic are ported from the
// original atmel-samd gb_m4 implementation; they are GameBoy-side and therefore
// portable.  Only the delivery mechanism (SAMD CCL + event system + port DMA)
// has been replaced with RP2350 PIO + DMA.

#include "shared-bindings/_gbio/__init__.h"
#include "common-hal/_gbio/__init__.h"

#include <string.h>

#include "bindings/rp2pio/StateMachine.h"
#include "common-hal/microcontroller/Pin.h"
#include "common-hal/microcontroller/__init__.h"
#include "py/runtime.h"
#include "shared-bindings/microcontroller/__init__.h"
#include "supervisor/shared/tick.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/structs/sio.h"

// ===== PIN MAP (PyGameBoy RP2350) =====
// game boy cartridge bus.  See boards/pygameboy_rp2350/pins.c.
#define GB_RD_PIN       4    // /RD  (input from GB)
#define GB_A0_PIN       6    // A0..A15 = GPIO6..GPIO21
#define GB_A15_PIN      21
#define GB_DATA_OE_PIN  28   // 74LVC4245 level shifter output enable
#define GB_D0_PIN       29   // D0..D7 = GPIO29..GPIO36 (3V side of U5)
#define GB_D7_PIN       36
#define GB_RESET_PIN    37   // assert (active high) drives /GB_RESET low via Q1

// Internal handshake pin shared between the two PIO state machines.  GPIO22
// lies in the 16..31 overlap of the two GPIO banks so both a base-0 and a
// base-16 state machine can see it.  Claimed by _gbio when CIRCUITPY_GBIO=1.
//
// NOTE on encoding `wait gpio`: when this program is loaded via the pico-SDK
// `pio_add_program_at_offset()` helper (which rp2pio_statemachine_construct
// uses), the SDK, with PICO_PIO_USE_GPIO_BASE defined (true on RP2350B with 48
// GPIOs), XORs each `wait gpio <n>` instruction's encoded 5-bit index with the
// PIO instance's GPIOBASE register (0 or 16) so that you encode the RAW
// ABSOLUTE GPIO number, and the hardware's own GPIOBASE pre-add then lands on
// the real pin.  So both state machines encode `wait gpio` with raw absolute
// pin numbers (e.g. /RD = 4, handshake = 22) regardless of which PIO base they
// live on -- exactly like the SM_PERIPHERAL `sm_config_set_*()` helpers, which
// also take absolute pin numbers and internally subtract the base.
#define GB_HANDSHAKE_PIN 22

// The level shifter output enable is driven statically so the RP2350 always
// drives the game boy data bus during reads (this cart never writes to GB
// cartridge RAM).  Set this to the level that ENABLES the shifter output.  The
// 74LVC4245 /OE is active low; flip to 1 if this board inverts it.
#define GB_DATA_OE_ASSERTED 0

// ===== SM83 COMMAND STREAMS (ported verbatim from gb_m4) =====

uint8_t gameboy_boot[] = {
    // Adafruit
    0x00, 0x30, 0x00, 0xC6, 0x00, 0x07, 0xCC, 0xCC, 0x00, 0xF1, 0x13, 0x3B, 0xC0, 0xD1, 0x00, 0xBD,
    0x00, 0x66, 0x00, 0x66, 0xC1, 0xDD, 0x08, 0xE8, 0x36, 0x63, 0xE6, 0xE6, 0xCC, 0xC7, 0xCD, 0xDC,
    0xF9, 0xBD, 0xBB, 0xBB, 0x11, 0x11, 0x88, 0x88, 0x66, 0x63, 0x66, 0xE6, 0xDD, 0xDD, 0x88, 0x8E,

    // Nintendo logo 48 bytes
    0xce, 0xed, 0x66, 0x66, 0xcc, 0x0d, 0x00, 0x0b, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0c, 0x00, 0x0d,
    0x00, 0x08, 0x11, 0x1f, 0x88, 0x89, 0x00, 0x0e, 0xdc, 0xcc, 0x6e, 0xe6, 0xdd, 0xdd, 0xd9, 0x99,
    0xbb, 0xbb, 0x67, 0x63, 0x6e, 0x0e, 0xec, 0xcc, 0xdd, 0xdc, 0x99, 0x9f, 0xbb, 0xb9, 0x33, 0x3e,

    // Cartridge header 28 bytes
    0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x57, 0x6F, 0x72, 0x6C, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xCA, 0x31, 0x58,

    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // Set the stack pointer
    0x31, 0x00, 0xe0,

    0x00, 0x00,

    // Set both lines low to detect any press.
    0xf0, 0x00,                             // LD A, FF00 - Poke the gpio by reading from it
    0x3e, 0x00,                             // LD A, 0x30
    0xe0, 0x00,                             // LD FF00, A

    0x00, 0x00,

    // Clear vsync and joypad interrupts
    0x3e, 0x00,                             // LD A, 0x0
    0xe0, 0x0f,                             // LD ff0f, A

    // Enable vsync and joypad interrupts
    0x3e, 0x11,                             // LD A, 0x11 (vblank is bit 0)
    0xe0, 0xff,                             // LD ffff, A

    0x00, 0x00,

    0xfb,                             // enable interrupts
    0xfb,                             // enable interrupts
    0xfb,                             // enable interrupts

    0x00, 0x00,

    // Load 0x1000 into hl and repeatedly jump to it until we do
    // something else. This prevents the program counter from
    // exiting the cartridge address range.
    0x21, 0x00, 0x10, 0xe9
};

uint8_t gameboy_color_boot[] = {
    // Nintendo logo 48 bytes to check first
    0xce, 0xed, 0x66, 0x66, 0xcc, 0x0d, 0x00, 0x0b, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0c, 0x00, 0x0d,
    0x00, 0x08, 0x11, 0x1f, 0x88, 0x89, 0x00, 0x0e, 0xdc, 0xcc, 0x6e, 0xe6, 0xdd, 0xdd, 0xd9, 0x99,
    0xbb, 0xbb, 0x67, 0x63, 0x6e, 0x0e, 0xec, 0xcc, 0xdd, 0xdc, 0x99, 0x9f, 0xbb, 0xb9, 0x33, 0x3e,

    // Adafruit
    0x00, 0x00, 0x30, 0x30, 0x00, 0x00, 0xC6, 0xC6, 0x00, 0x00, 0x07, 0x07, 0xCC, 0xCC, 0xCC, 0xCC,
    0x00, 0x00, 0xF1, 0xF1, 0x13, 0x13, 0x3B, 0x3B, 0xC0, 0xC0, 0xD1, 0xD1, 0x00, 0x00, 0xBD, 0xBD,
    0x00, 0x00, 0x66, 0x66, 0x00, 0x00, 0x66, 0x66, 0xC1, 0xC1, 0xDD, 0xDD, 0x08, 0x08, 0xE8, 0xE8,
    0x36, 0x36, 0x63, 0x63, 0xE6, 0xE6, 0xE6, 0xE6, 0xCC, 0xCC, 0xC7, 0xC7, 0xCD, 0xCD, 0xDC, 0xDC,
    0xF9, 0xF9, 0xBD, 0xBD, 0xBB, 0xBB, 0xBB, 0xBB, 0x11, 0x11, 0x11, 0x11, 0x88, 0x88, 0x88, 0x88,
    0x66, 0x66, 0x63, 0x63, 0x66, 0x66, 0xE6, 0xE6, 0xDD, 0xDD, 0xDD, 0xDD, 0x88, 0x88, 0x8E, 0x8E,

    // Licensee code - Nintendo R&D1
    0x33,                                 // 0x14b
    0x30,                                 // 0x144 "0"
    0x31,                                 // 0x145 "1"

    // Cartridge header 28 bytes
    // 11 Game title characters - 0x134 - 0x13e
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // Game code 0x13f - 0x142
    0x20, 0x20, 0x20, 0x20,

    // 0x143 * 69, these are to determine whether to do color selection during start up animation. read each other frame
    0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0,
    0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0,
    0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0,
    0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0,
    0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0,
    0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0,
    0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0,

    // Cartridge header 26 bytes w/ valid checksum
    0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x57, 0x6F, 0x72, 0x6C, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, // Last is 0x143 but only used in checksum
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xCA,

    0xc0,                             // The very last read is 0x143 to set the mode of the GBC. This is the one that matters!

    0x00,

    // Set the stack pointer
    0x31, 0x00, 0xe0,

    0x00, 0x00,

    // Set both lines low to detect any press.
    0xf0, 0x00,                             // LD A, FF00 - Poke the gpio by reading from it
    0x3e, 0x00,                             // LD A, 0x30
    0xe0, 0x00,                             // LD FF00, A

    0x00, 0x00,

    // Clear vsync and joypad interrupts
    0x3e, 0x00,                             // LD A, 0x0
    0xe0, 0x0f,                             // LD ff0f, A

    // Enable vsync and joypad interrupts
    0x3e, 0x11,                             // LD A, 0x11 (vblank is bit 0)
    0xe0, 0xff,                             // LD ffff, A

    0x00, 0x00,

    0xfb,                             // enable interrupts
    0xfb,                             // enable interrupts
    0xfb,                             // enable interrupts

    0x00, 0x00,

    // Load 0x1000 into hl and repeatedly jump to it until we do
    // something else. This prevents the program counter from
    // exiting the cartridge address range.
    0x21, 0x00, 0x10, 0xe9
};

uint8_t gamepad_interrupt_response[] = {
    0x00, 0x00,
    0x16, 0x30, // Load 0x30 into D
    0x0e, 0x00, // Load 0x00 into C

    0x3e, 0x20, // Turn on only one column
    0xe2, // Load A into 0xff00
    0x00, 0x00, 0x00, 0x00, 0x00,
    0xf2, // read register 0xff + C into A
    0x5f, // Put A into E
    0x3e, 0x10, // Turn on the other column
    0xe2, // Load A into 0xff00
    0x1a, 0x00, // Load dummy from (DE) into

    0x00, 0x00, 0x00, 0x00, 0x00,

    0x14, // Increment D
    0xf2, // read register 0xff + C into A
    0x5f, // Put A into E
    0x1a, 0x00, // Load dummy from (DE)

    0x3e, 0x00, // Turn on both columns so any press is detected
    0xe2, // Load A into 0xff00
    0x21, 0x00, 0x11, // Load 0x1100 into hl
    0x3e, 0xef, // Clear the interrupt before it's enabled
    0x0e, 0x0f, // Load 0x0f into C
    0xe2, // Load A into 0xff0f

    0xd9, // Return from the interrupt
    0xe9
};

uint8_t fetch_gamepad_commands[] = {
    0x16, 0x30,                          // Load 0x30 into D
    0x0e, 0x00,                           // Load 0x00 into C

    0x3e, 0x20,                          // Turn on only one column
    0xe2,                          // Load A into 0xff00
    0x00, 0x00, 0x00, 0x00, 0x00,
    0xf2,                          // read register 0xff + C into A
    0x5f,                          // Put A into E
    0x3e, 0x10,                          // Turn on the other column
    0xe2,                          // Load A into 0xff00
    0x1a, 0x00,                          // Load dummy from (DE) into

    0x14,                          // Increment D
    0x00, 0x00, 0x00, 0x00, 0x00,
    0xf2,                          // read register 0xff + C into A
    0x5f,                          // Put A into E
    0x1a, 0x00,                          // Load dummy from (DE)

    0x3e, 0x00,                          // Turn on both columns so any press is detected
    0xe2,                          // Load A into 0xff00
    0x3e, 0xef,                          // Clear the interrupt before it's enabled
    0x0e, 0x0f,                           // Load 0x0f into C
    0xe2,                          // Load A into 0xff0f
};

uint8_t change_screen_commands[] = {
    0x00,     // Noop to sync DMA to GB clock
    0x0e,     // Load next value into C
    0x40,     // LCDC register
    0x3e,     // Load next value into A
    0x91,     // Default value out of bootloader
    0xe2
};            // Load A into 0xff00 + C

// ===== STATE =====

// Idle address the game boy spins on between command sequences.  All command
// streams end by loading this into HL and executing JP (HL), leaving 0xE9
// (JP (HL)) on the bus so the game boy re-reads it forever.
#define GB_IDLE_ADDR 0x1300

static rp2pio_statemachine_obj_t ctrl_sm;   // watches /RD + A15, pulses handshake
static rp2pio_statemachine_obj_t data_sm;   // drives D0..D7 from its TX FIFO

static int dma_chan = -1;                    // feeds data_sm TX FIFO from RAM

// Command buffers (in ordinary RAM; the SAMD port used backup RAM but the
// RP2350 keeps these in normal SRAM and re-arms them on reset_gameboy).
static uint8_t command_cache[1024] __attribute__((aligned(4)));
static uint8_t vblank_interrupt_response[1024] __attribute__((aligned(4)));

static uint8_t gamepad_state = 0xff;
static volatile uint32_t vsync_count = 0;
static volatile uint64_t last_vsync_time = 0;

// Length (in bytes) of the currently staged vblank response, including the
// leading [NOP, JP(HL)] prefix and the trailing idle-restore epilogue.
static volatile uint16_t vblank_response_length;
static volatile uint32_t total_additional_cycles;
static volatile bool updating_vblank_response;

static volatile bool everything_going = false;     // boot complete?
static volatile bool gameboy_color_booting = false;
static volatile bool gameboy_color = false;

// Set true while the /RD address-dispatch interrupt should fire.  It is only
// enabled on demand (boot detection, vblank waits, gamepad fetch) so the ARM
// is free while the game boy idles.
static volatile bool address_dispatch_enabled;

// Boot: count of remaining reads during which we look for the GBC signature.
static volatile uint16_t boot_detect_remaining;
static volatile uint16_t boot_address_i;

// ===== PIO PROGRAMS =====
//
// Built at runtime with the pio_encode_* helpers so we can't accidentally
// hand-encode them wrong.  Each is small enough to fit in one PIO instruction
// memory slot.

#define CTRL_PROG_LEN 6
#define DATA_PROG_LEN 5
static uint16_t ctrl_program[CTRL_PROG_LEN];
static uint16_t data_program[DATA_PROG_LEN];

static void build_pio_programs(void) {
    // gbio_ctrl (GPIO base 0).  jmp_pin = A15 (GPIO21).  set pin = handshake
    // (GPIO22).  wait gpio 4 = /RD.
    //
    //   loop:                          ; index 0
    //     jmp pin loop                 ; while A15 high (not a ROM read) spin
    //     wait 0 gpio 4                ; wait for /RD falling (read start)
    //     set pins, 1                  ; raise handshake -> data SM advances
    //     wait 1 gpio 4                ; wait for /RD rising (read done)
    //     set pins, 0                  ; lower handshake
    //     jmp loop
    ctrl_program[0] = pio_encode_jmp_pin(0);
    ctrl_program[1] = pio_encode_wait_gpio(false, GB_RD_PIN);
    ctrl_program[2] = pio_encode_set(pio_pins, 1);
    ctrl_program[3] = pio_encode_wait_gpio(true, GB_RD_PIN);
    ctrl_program[4] = pio_encode_set(pio_pins, 0);
    ctrl_program[5] = pio_encode_jmp(0);

    // gbio_data.  out pins = D0..D7 (GPIO29..36).  `wait gpio` references the
    // handshake pin (GPIO22) by its raw absolute number; the pico-SDK loader
    // (see the GB_HANDSHAKE_PIN note above) reorganises the encoded index to
    // match whatever PIO base this SM ends up on.
    //
    //   pull block              ; (entry) prefetch byte[0] into OSR
    // loop:
    //     out pins, 8           ; drive the current byte onto D0..D7 now, so it
    //                           ;  is stable on the bus BEFORE the read strobe
    //     wait 1 gpio 22        ; read in progress (handshake high); byte held
    //     wait 0 gpio 22        ; read finished (handshake low)
    //     jmp pull               ; prefetch the next byte; pins still hold the
    //                           ;  current one until the next `out`
    //
    // Driving the byte BEFORE the /RD strobe (instead of reacting to it) is what
    // makes the data valid for the GameBoy's sampling edge regardless of where
    // in the read cycle that edge falls -- the previous `wait 0 -> jmp -> pull
    // -> out` reload happens in the dead time between reads, so by the time the
    // next /RD falls the new byte has already been on the bus for a while.  This
    // mirrors the SAMD gb_m4 port, whose port-DMA latches the output register
    // on the RISING edge of the read-valid LUT (end of the previous read), so it
    // is stable throughout the next.  Without this the byte arrived a few PIO
    // cycles after /RD fell, racing the GameBoy's data-sample point and
    // corrupting the boot logo.
    //
    // NOTE: because this drives the byte BEFORE its read (no delivery latency),
    // but the SM83 boot stream is authored for SAMD's one-read pipeline delay
    // (read#1 receives the output register's initial value, stream byte[0]
    // lands on read#2), callers prepend a single 0x00 byte to the DMG boot feed
    // to reproduce that delay exactly -- see common_hal_gbio_reset_gameboy().
    data_program[0] = pio_encode_pull(false, true);
    data_program[1] = pio_encode_out(pio_pins, 8);
    data_program[2] = pio_encode_wait_gpio(true, GB_HANDSHAKE_PIN);
    data_program[3] = pio_encode_wait_gpio(false, GB_HANDSHAKE_PIN);
    data_program[4] = pio_encode_jmp(0);
}

// ===== DMA / FEEDER HELPERS =====

static void feeder_start(const uint8_t *buf, size_t len) {
    // Stop anything in flight, drain any bytes left in the TX FIFO (e.g. from a
    // mid-stream abort), then pump `len` bytes from `buf` into the data state
    // machine's TX FIFO, paced by the SM's TX DREQ.
    mp_printf(&mp_plat_print, "  [gbio] feeder_start: len=%u\n", (unsigned)len);
    dma_channel_abort(dma_chan);
    pio_sm_drain_tx_fifo(data_sm.pio, data_sm.state_machine);
    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, pio_get_dreq(data_sm.pio, data_sm.state_machine, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    // out_shift_right == true -> bytes go to the low lane of txf.
    volatile uint8_t *txf = (volatile uint8_t *)&data_sm.pio->txf[data_sm.state_machine];
    dma_channel_configure(dma_chan, &c,
        txf,
        buf,
        len,
        true /* start immediately */);
    mp_printf(&mp_plat_print, "  [gbio] feeder_start: armed\n");
}

static void feeder_wait_drained(void) {
    // Wait for the DMA to finish pushing bytes into the FIFO, then for the
    // state machine to consume every one of them (it stalls on `out` when the
    // FIFO is empty -> TXSTALL asserts).
    mp_printf(&mp_plat_print, "  [gbio] feeder_wait_drained: waiting DMA idle\n");
    uint32_t spins = 0;
    while (dma_channel_is_busy(dma_chan)) {
        RUN_BACKGROUND_TASKS;
        if (++spins > 1000000) {
            mp_printf(&mp_plat_print, "  [gbio] feeder_wait_drained: DMA STILL BUSY after 1M spins (trans_count=%u)\n",
                (unsigned)dma_channel_hw_addr(dma_chan)->transfer_count);
            spins = 0;
        }
    }
    mp_printf(&mp_plat_print, "  [gbio] feeder_wait_drained: DMA idle, waiting SM TXSTALL\n");
    uint32_t stall_mask = 1u << (PIO_FDEBUG_TXSTALL_LSB + data_sm.state_machine);
    data_sm.pio->fdebug = stall_mask;      // clear
    spins = 0;
    while ((data_sm.pio->fdebug & stall_mask) == 0) {
        // Still draining.  The game boy paces this at ~1us/byte.
        RUN_BACKGROUND_TASKS;
        if (++spins > 1000000) {
            mp_printf(&mp_plat_print, "  [gbio] feeder_wait_drained: SM NOT stalling after 1M spins (fdebug=0x%08x)\n",
                (unsigned)data_sm.pio->fdebug);
            spins = 0;
        }
    }
    mp_printf(&mp_plat_print, "  [gbio] feeder_wait_drained: drained\n");
}

// ===== ADDRESS DISPATCH (ARM, on-demand) =====
//
// A /RD falling edge fires this handler.  It reads A0..A15 (GPIO6..21) from
// SIO and reacts to the few addresses that matter: the vblank (0x0040) and
// joypad (0x0060) interrupt vectors, the 0x3000-0x3FFF gamepad-read encoding
// region, and -- during boot -- the game boy Color detection pattern.

static inline uint16_t read_gb_address(void) {
    return (uint16_t)((sio_hw->gpio_in >> GB_A0_PIN) & 0xffff);
}

static void gbio_rd_dispatch(void) {
    uint16_t address = read_gb_address();

    if (!everything_going) {
        // Boot: look for the game boy Color signature for the first few reads.
        if (boot_detect_remaining) {
            boot_detect_remaining--;
            boot_address_i++;
            if (boot_address_i == 50 && address == 0x0104) {
                gameboy_color_booting = true;
            }
            if (boot_detect_remaining == 0) {
                gpio_set_irq_enabled(GB_RD_PIN, GPIO_IRQ_EDGE_FALL, false);
                address_dispatch_enabled = false;
            }
        }
        return;
    }

    if (address == 0x0040) {
        // VBlank interrupt vector.
        vsync_count++;
        last_vsync_time = supervisor_ticks_ms64();
        if (!updating_vblank_response && vblank_response_length > 2 &&
            !dma_channel_is_busy(dma_chan)) {
            // Inject the staged vblank response.  Copy the staged prefix +
            // commands into command_cache, append the idle-restore epilogue,
            // and re-arm the feeder.  The data SM is stalled on `pull`
            // holding the idle byte; refilling the FIFO makes it serve
            // response[0] on this very read (if we win the race against /RD
            // rising -- see the file header).
            uint16_t len = vblank_response_length;
            memcpy(command_cache, vblank_interrupt_response, len);
            command_cache[len + 0] = 0xd1;     // POP DE (don't leak the frame)
            command_cache[len + 1] = 0x21;     // LD HL, GB_IDLE_ADDR
            command_cache[len + 2] = (uint8_t)(GB_IDLE_ADDR & 0xff);
            command_cache[len + 3] = (uint8_t)(GB_IDLE_ADDR >> 8);
            command_cache[len + 4] = 0xfb;     // EI
            command_cache[len + 5] = 0xfb;     // EI
            command_cache[len + 6] = 0xe9;     // JP (HL) -> idle
            len += 7;
            vblank_response_length = 2;       // back to the [NOP, JP(HL)] prefix
            total_additional_cycles = 0;
            feeder_start(command_cache, len);
        }
    } else if (address == 0x0060) {
        // Joypad interrupt vector.  Re-arm the canned response if we're idle.
        if (!dma_channel_is_busy(dma_chan)) {
            feeder_start(gamepad_interrupt_response, sizeof(gamepad_interrupt_response));
        }
    } else if ((address & 0xf000) == 0x3000) {
        // The game boy encodes the joypad register value into the low byte of a
        // dummy cartridge read at 0x30xx / 0x31xx.  Decode it back into a
        // pressed-button bitmask (bits low == pressed).
        uint8_t nibble = (uint8_t)(address & 0xf);
        uint8_t position = (uint8_t)((address & 0x0100) >> 8);
        uint8_t data = 0xf0 | nibble;
        if (position == 1) {
            data = 0x0f | (uint8_t)(nibble << 4);
        }
        gamepad_state &= data;
    }
}

static void enable_address_dispatch(void) {
    if (address_dispatch_enabled) {
        return;
    }
    address_dispatch_enabled = true;
    // Acknowledge any pending edge before arming.
    gpio_acknowledge_irq(GB_RD_PIN, GPIO_IRQ_EDGE_FALL);
    gpio_set_irq_enabled(GB_RD_PIN, GPIO_IRQ_EDGE_FALL, true);
}

static void disable_address_dispatch(void) {
    if (!address_dispatch_enabled) {
        return;
    }
    gpio_set_irq_enabled(GB_RD_PIN, GPIO_IRQ_EDGE_FALL, false);
    address_dispatch_enabled = false;
}

// ===== INIT =====

static bool gbio_inited = false;

void gbio_init(void) {
    if (gbio_inited) {
        return;
    }
    build_pio_programs();

    // Address bus A0..A14 as plain SIO inputs (A15 is owned by the ctrl SM).
    // sio_hw->gpio_in reads them regardless of pin function.
    for (uint8_t p = GB_A0_PIN; p < GB_A15_PIN; p++) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
        gpio_set_pulls(p, false, false);
    }
    // /RD as an input with a falling-edge interrupt handler registered (armed
    // only on demand).  It is also watched by the ctrl SM via `wait gpio`, but
    // pad-level interrupt detection works independently of pin function.
    gpio_init(GB_RD_PIN);
    gpio_set_dir(GB_RD_PIN, GPIO_IN);
    gpio_set_pulls(GB_RD_PIN, false, false);
    gpio_add_raw_irq_handler(GB_RD_PIN, gbio_rd_dispatch);

    // /GB_RESET (active high to assert) and level-shifter OE as GPIO outputs.
    gpio_init(GB_RESET_PIN);
    gpio_set_dir(GB_RESET_PIN, GPIO_OUT);
    gpio_put(GB_RESET_PIN, 0);            // de-assert reset (GB running)
    gpio_init(GB_DATA_OE_PIN);
    gpio_set_dir(GB_DATA_OE_PIN, GPIO_OUT);
    gpio_put(GB_DATA_OE_PIN, GB_DATA_OE_ASSERTED); // enable RP->GB data output

    // --- ctrl SM (GPIO base 0): watches /RD + A15, pulses the handshake. ---
    const mcu_pin_obj_t *a15_pin = mcu_get_pin_by_number(GB_A15_PIN);
    const mcu_pin_obj_t *hs_pin = mcu_get_pin_by_number(GB_HANDSHAKE_PIN);

    pio_pinmask_t ctrl_pins = PIO_PINMASK_NONE;
    PIO_PINMASK_SET(ctrl_pins, GB_RD_PIN);
    PIO_PINMASK_SET(ctrl_pins, GB_A15_PIN);
    PIO_PINMASK_SET(ctrl_pins, GB_HANDSHAKE_PIN);

    bool ok = rp2pio_statemachine_construct(&ctrl_sm,
        ctrl_program, CTRL_PROG_LEN,
        0 /* frequency: use clk_sys */,
        NULL, 0,                                   // init
        NULL, 0,                                   // out
        NULL, 0,                                   // in
        PIO_PINMASK_NONE, PIO_PINMASK_NONE,        // pull up/down
        hs_pin, 1,                                 // set pin = handshake
        NULL, 0, false,                            // sideset
        PIO_PINMASK_FROM_VALUE(0),                 // initial pin state
        PIO_PINMASK_FROM_PIN(GB_HANDSHAKE_PIN),    // handshake is an output
        a15_pin,                                   // jmp pin = A15
        ctrl_pins,
        false, false,                              // no tx/rx fifo
        false, 0, false,                           // auto_pull, threshold, shift
        false,                                     // wait_for_txstall
        false, 0, false,                           // auto_push
        true,                                      // claim pins
        false,                                     // not user interruptible
        false,                                     // sideset_enable
        0, CTRL_PROG_LEN - 1, PIO_ANY_OFFSET,      // wrap
        PIO_FIFO_TYPE_DEFAULT,
        PIO_MOV_STATUS_DEFAULT, PIO_MOV_N_DEFAULT);
    if (!ok) {
        mp_printf(&mp_plat_print, "gbio: ctrl state machine init failed\n");
        return;
    }
    rp2pio_statemachine_never_reset(ctrl_sm.pio, ctrl_sm.state_machine);

    // --- data SM (GPIO base 16): drives D0..D7 from its TX FIFO. ---
    const mcu_pin_obj_t *d0_pin = mcu_get_pin_by_number(GB_D0_PIN);

    pio_pinmask_t data_pins = PIO_PINMASK_NONE;
    // NOTE: the handshake pin (GPIO22) is intentionally NOT in this mask. It is
    // driven by the ctrl SM (which claims it) and the data SM only *reads* it
    // with `wait gpio`, which samples the raw pad level regardless of pin
    // function. Declaring it here would make rp2pio reject the data SM because
    // the pin is already owned by the ctrl SM's PIO instance.
    for (uint8_t p = GB_D0_PIN; p <= GB_D7_PIN; p++) {
        PIO_PINMASK_SET(data_pins, p);
    }

    ok = rp2pio_statemachine_construct(&data_sm,
        data_program, DATA_PROG_LEN,
        0,
        NULL, 0,
        d0_pin, 8,                                // out pins = D0..D7
        NULL, 0,                                   // in
        PIO_PINMASK_NONE, PIO_PINMASK_NONE,        // pull
        NULL, 0,                                   // set
        NULL, 0, false,                            // sideset
        PIO_PINMASK_FROM_VALUE(0),                 // initial pin state (D low)
        PIO_PINMASK_FROM_VALUE(((uint64_t)0xff) << GB_D0_PIN), // D0..D7 outputs
        NULL,                                      // no jmp pin
        data_pins,
        true, false,                              // tx fifo yes, rx no
        false, 8, true,                            // manual pull (no auto_pull); shift right (LSB->D0)
        false,                                    // wait_for_txstall
        false, 0, false,                          // auto_push
        true,                                     // claim pins (shared handshake handled by refcount)
        false,                                     // not user interruptible
        false,                                     // sideset_enable
        0, DATA_PROG_LEN - 1, PIO_ANY_OFFSET,
        PIO_FIFO_TYPE_DEFAULT,
        PIO_MOV_STATUS_DEFAULT, PIO_MOV_N_DEFAULT);
    if (!ok) {
        mp_printf(&mp_plat_print, "gbio: data state machine init failed\n");
        return;
    }
    rp2pio_statemachine_never_reset(data_sm.pio, data_sm.state_machine);

    // DMA channel feeding the data SM's TX FIFO.  Claimed once, reused.
    dma_chan = (int)dma_claim_unused_channel(false);
    if (dma_chan < 0) {
        mp_printf(&mp_plat_print, "gbio: no free DMA channel\n");
    }

    // Reset vblank response to the idle-passthrough prefix: NOP; JP (HL).
    vblank_interrupt_response[0] = 0x00;
    vblank_interrupt_response[1] = 0xe9;
    vblank_response_length = 2;
    total_additional_cycles = 0;

    // Keep our pins across VM resets; reset_port() resets GPIO/PIO generally.
    never_reset_pin_number(GB_RD_PIN);
    for (uint8_t p = GB_A0_PIN; p <= GB_A15_PIN; p++) {
        never_reset_pin_number(p);
    }
    never_reset_pin_number(GB_HANDSHAKE_PIN);
    never_reset_pin_number(GB_DATA_OE_PIN);
    never_reset_pin_number(GB_RESET_PIN);
    for (uint8_t p = GB_D0_PIN; p <= GB_D7_PIN; p++) {
        never_reset_pin_number(p);
    }

    gbio_inited = true;
}

// ===== PUBLIC API =====

void common_hal_gbio_reset_gameboy(void) {
    if (!gbio_inited) {
        mp_printf(&mp_plat_print, "GBIO not initialized\n");
        return;
    }
    mp_printf(&mp_plat_print, "Resetting game boy...\n");

    // Hold the game boy in reset while we arm the boot stream.
    mp_printf(&mp_plat_print, "  [gbio] stage 1: asserting /GB_RESET\n");
    gpio_put(GB_RESET_PIN, 1);             // assert /GB_RESET
    common_hal_mcu_delay_us(10);

    everything_going = false;
    gameboy_color_booting = false;
    vsync_count = 0;
    gamepad_state = 0xff;
    boot_address_i = 0;

    // Watch the first ~64 reads to detect a game boy Color.
    boot_detect_remaining = 64;
    mp_printf(&mp_plat_print, "  [gbio] stage 2: enabling address dispatch\n");
    enable_address_dispatch();

    // Feed the DMG boot stream first.  If we detect a GBC we re-boot below.
    //
    // Pipeline-delay alignment with the SAMD gb_m4 port: on SAMD the data DMA
    // is triggered on the RISING edge of the read-valid LUT (i.e. at the END of
    // each cart read), so the byte loaded at the end of read N-1 is what the
    // game boy samples during read N.  That means the very first cart read after
    // /GB_RESET release samples the output register's initial value (0), and
    // gameboy_boot[0] (a deliberately-early 0x00) lands on read #2.  The entire
    // stream is authored for that one-read pipeline delay.
    //
    // The RP2350 PIO has no such delay: its program is `pull; wait handshake;
    // out; wait`, so gameboy_boot[0] would be served on read #1 -- one read
    // earlier than the stream expects -- which misaligns the Nintendo logo /
    // header checksum and the DMG boot ROM halts partway through reading it.
    //
    // Mirror SAMD's read #1 by prepending a single 0x00 byte to the fed buffer,
    // so the RP2350 serves: read#1 = 0x00 (the prepended byte), read#2 =
    // gameboy_boot[0], ... exactly reproducing the SAMD byte/address sequence.
    // (The GBC re-arm path below needs NO such prepend: SAMD software-triggers
    // the DMA there, which already has no delay, and gameboy_color_boot[0] is
    // the first logo byte 0xCE -- so the RP2350, also delay-free, matches.)
    //
    // command_cache is free here: nothing_going (everything_going is false)
    // so no vblank / queue_commands path can touch it concurrently.
    mp_printf(&mp_plat_print, "  [gbio] stage 3: starting DMG feeder (gameboy_boot)\n");
    command_cache[0] = 0x00;     // SAMD-equivalent initial-output read
    memcpy(command_cache + 1, gameboy_boot, sizeof(gameboy_boot));
    feeder_start(command_cache, sizeof(gameboy_boot) + 1);

    // Release reset: the game boy boot ROM starts reading the cartridge header.
    mp_printf(&mp_plat_print, "  [gbio] stage 4: releasing /GB_RESET\n");
    gpio_put(GB_RESET_PIN, 0);

    // Watch the DMA.  If the GBC signature shows up early, immediately re-arm
    // with the GBC boot stream (mirroring the original gb_m4 port) so the GBC
    // boot ROM never sees enough of the wrong logo to hang.
    bool first_init = true;
    mp_printf(&mp_plat_print, "  [gbio] stage 5: waiting for initial DMA drain (DMG stream)\n");
    uint32_t dma_spins = 0;
    while (dma_channel_is_busy(dma_chan)) {
        RUN_BACKGROUND_TASKS;
        if (gameboy_color_booting && first_init) {
            mp_printf(&mp_plat_print, "  [gbio] stage 5a: GBC detected -> re-arming GBC boot stream\n");
            first_init = false;
            gpio_put(GB_RESET_PIN, 1);     // re-assert /GB_RESET
            common_hal_mcu_delay_us(10);
            disable_address_dispatch();
            // Reset the data SM (clears OSR / shift counters) so no stale DMG
            // byte leaks into the GBC boot stream.
            pio_sm_restart(data_sm.pio, data_sm.state_machine);
            feeder_start(gameboy_color_boot, sizeof(gameboy_color_boot));
            gpio_put(GB_RESET_PIN, 0);
        }
        if (++dma_spins > 1000000) {
            mp_printf(&mp_plat_print, "  [gbio] stage 5: DMA STILL BUSY after 1M spins (transfer_count=%u, gbc=%d)\n",
                (unsigned)dma_channel_hw_addr(dma_chan)->transfer_count, (int)gameboy_color_booting);
            dma_spins = 0;
        }
    }
    mp_printf(&mp_plat_print, "  [gbio] stage 5: initial DMA drain complete (first_init=%d)\n", (int)first_init);

    // Wait for whichever stream is active to be fully consumed.
    mp_printf(&mp_plat_print, "  [gbio] stage 6: feeder_wait_drained on active stream\n");
    feeder_wait_drained();

    if (!first_init) {
        gameboy_color = true;
    } else {
        gameboy_color = false;
    }
    mp_printf(&mp_plat_print, "  [gbio] stage 7: disabling address dispatch (gameboy_color=%d)\n", (int)gameboy_color);
    disable_address_dispatch();

    // Reset the vblank passthrough.
    vblank_interrupt_response[0] = 0x00;
    vblank_interrupt_response[1] = 0xe9;
    vblank_response_length = 2;
    total_additional_cycles = 0;

    last_vsync_time = supervisor_ticks_ms64();
    everything_going = true;
    mp_printf(&mp_plat_print, "  [gbio] stage 8: everything_going=true\n");
    mp_printf(&mp_plat_print, "game boy reset complete\n");
}

void common_hal_gbio_queue_commands(const uint8_t *buf, uint32_t len) {
    if (!gbio_inited || dma_chan < 0) {
        return;
    }
    if (len > sizeof(command_cache) - 5 - 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("Too many commands"));
    }
    if (!everything_going || supervisor_ticks_ms64() - last_vsync_time > 600) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("game boy not running"));
    }

    feeder_wait_drained();

    // Disable game boy interrupts while we swap in new code (an interrupt
    // mid-sequence would read our bytes as a garbage vector), then run the
    // user's commands, then restore the idle spin on GB_IDLE_ADDR.
    uint32_t total_len = 0;
    command_cache[total_len++] = 0x00;     // noop (DMA sync)
    command_cache[total_len++] = 0xf3;     // DI (disable interrupts)
    command_cache[total_len++] = 0x00;     // noop while DI takes effect
    memcpy(command_cache + total_len, buf, len);
    total_len += len;
    command_cache[total_len++] = 0x21;     // LD HL, GB_IDLE_ADDR
    command_cache[total_len++] = (uint8_t)(GB_IDLE_ADDR & 0xff);
    command_cache[total_len++] = (uint8_t)(GB_IDLE_ADDR >> 8);
    command_cache[total_len++] = 0xfb;     // EI
    command_cache[total_len++] = 0xfb;     // EI (delayed by one insn)
    command_cache[total_len++] = 0xe9;     // JP (HL) -> idle spin on GB_IDLE_ADDR

    feeder_start(command_cache, total_len);
    feeder_wait_drained();
}

void common_hal_gbio_queue_vblank_commands(const uint8_t *buf, uint32_t len, uint32_t additional_cycles) {
    if (!gbio_inited || dma_chan < 0) {
        return;
    }
    if (!everything_going || supervisor_ticks_ms64() - last_vsync_time > 600) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("game boy not running"));
    }
    // The vblank response buffer holds [NOP, JP(HL)] + the user commands.  The
    // idle-restore epilogue is appended at serve time (in the /RD handler on
    // 0x0040) so repeated calls accumulate commands without piling up
    // epilogues.  Append as much as fits; if there's no room, wait a frame and
    // start fresh (the staged response is consumed by a vblank first).
    if (dma_channel_is_busy(dma_chan)) {
        // A response is currently being served; wait for it to finish.
        while (dma_channel_is_busy(dma_chan)) {
            RUN_BACKGROUND_TASKS;
        }
    }
    uint32_t remaining = sizeof(vblank_interrupt_response) - vblank_response_length - 7 /* epilogue */;
    if (len > remaining) {
        common_hal_gbio_wait_for_vblank();
        while (dma_channel_is_busy(dma_chan)) {
            RUN_BACKGROUND_TASKS;
        }
        // The previous response was served; the ISR reset the prefix.
    }

    updating_vblank_response = true;
    memcpy(vblank_interrupt_response + vblank_response_length, buf, len);
    vblank_response_length += len;
    total_additional_cycles += additional_cycles;
    updating_vblank_response = false;

    // Watch for the next 0x0040 read so the handler can swap the feeder over
    // to the staged response.  This raises the /RD interrupt at the GameBoy's
    // read rate (see the file header) for the duration of the wait.
    enable_address_dispatch();
}

void common_hal_gbio_set_lcdc(uint8_t value) {
    uint8_t previous = change_screen_commands[4];
    change_screen_commands[4] = value;
    // Turning the LCD off must happen during vblank to avoid tearing.
    if ((previous & 0x80) == 0x80 && (value & 0x80) == 0) {
        common_hal_gbio_queue_vblank_commands(change_screen_commands, sizeof(change_screen_commands), 1);
        common_hal_gbio_wait_for_vblank();
        return;
    }
    common_hal_gbio_queue_commands(change_screen_commands, sizeof(change_screen_commands));
}

uint8_t common_hal_gbio_get_lcdc(void) {
    return change_screen_commands[4];
}

void common_hal_gbio_wait_for_vblank(void) {
    if (!gbio_inited) {
        return;
    }
    if (!everything_going || supervisor_ticks_ms64() - last_vsync_time > 600) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("game boy not running"));
    }
    uint32_t start = vsync_count;
    enable_address_dispatch();
    while (start == vsync_count) {
        RUN_BACKGROUND_TASKS;
    }
    disable_address_dispatch();
}

uint32_t common_hal_gbio_get_vsync_count(void) {
    return vsync_count;
}

uint8_t common_hal_gbio_get_pressed(void) {
    if (!gbio_inited) {
        return 0xff;
    }
    uint8_t current = gamepad_state;
    gamepad_state = 0xff;
    // The fetch reads the joypad register and then does dummy cartridge reads
    // at 0x30xx whose address encodes the button state; the dispatch handler
    // decodes them into gamepad_state.  Watch /RD only for the duration of the
    // fetch sequence.
    enable_address_dispatch();
    common_hal_gbio_queue_commands(fetch_gamepad_commands, sizeof(fetch_gamepad_commands));
    disable_address_dispatch();
    return current;
}

bool common_hal_gbio_is_color(void) {
    return gameboy_color;
}
