// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// game boy cartridge interface for the PyGameBoy RP2350.
//
// The RP2350 plays the role of a game boy cartridge: it feeds a stream of SM83
// opcodes to the game boy CPU one byte at a time, synchronised to the game boy's
// own read strobes.  A single PIO state machine does the real-time work so the
// ARM core is free to run Python / USB between game boy accesses:
//
//   gbio_data  drives D0..D7 from its TX FIFO.  A DMA channel keeps the TX FIFO
//     full from a command buffer in RAM.  gbio_data directly watches /RD and
//     A15: it spins while A15 is high (non-ROM region), waits for /RD to fall (a
//     cartridge ROM read at A15 low), lets the read complete, then pulls the
//     next byte from the FIFO and drives it onto D0..D7 BEFORE its matching /RD
//     strobe (mirroring the SAMD port, whose port-DMA latches the output
//     register at the END of the previous read so the byte is stable throughout
//     the next), so the GameBoy's sample point always catches the right byte.
//     When the FIFO runs dry the state machine stalls on `pull` and the pins
//     hold the last byte -- this is the "idle" state in which the game boy spins
//     on a JP (HL) self loop.  The level-shifter output-enable (DATA_OE) is a
//     sideset output of this same SM, so the 74LVC4245 only drives D0..D7 onto
//     the game boy bus during ROM reads (and the idle spin) -- it is held
//     high-Z (OE de-asserted) outside the ROM region and while the GB is in
//     reset, instead of permanently outputting.
//
// A single state machine is sufficient because, on the v8 PyGameBoy PCB, every
// cartridge bus signal lives in the first 32 GPIO bank: A0..A15 = GPIO2..GPIO17,
// /RD = GPIO20 and D0..D7 = GPIO23..GPIO30.  (The earlier board routed D7 to
// GPIO36 and /RD to GPIO4, forcing two PIO instances linked by a physical
// handshake pin in the 16..31 bank overlap; that is no longer needed.)
//
// The ARM core still gets a /RD falling-edge interrupt (on demand) for address
// dispatch: it watches the vblank (0x0040) and joypad (0x0060) interrupt
// vectors, the 0x3000-0x3FFF gamepad-read encoding region, and -- during boot
// -- the game boy Color detection pattern, swapping the DMA feeder between
// command streams.  This runs in parallel with gbio_data and does not gate it.
//
// The SM83 command streams and the address-dispatch logic are ported from the
// original atmel-samd gb_m4 implementation; they are GameBoy-side and therefore
// portable.  Only the delivery mechanism (SAMD CCL + event system + port DMA,
// or the earlier two-PIO + handshake design) has been replaced with a single
// RP2350 PIO state machine + DMA.

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

// ===== PIN MAP (PyGameBoy RP2350, v8 PCB) =====
// game boy cartridge bus.  See boards/pygameboy_rp2350/pins.c.
// On the v8 PCB every cartridge signal lives in the first 32 GPIO bank, so a
// single PIO state machine (GPIO base 0) can watch /RD and A15 AND drive D0..D7
// -- no second state machine or inter-SM handshake pin is needed.
#define GB_RD_PIN       20   // /RD  (input from GB)
#define GB_A0_PIN       2    // A0..A15 = GPIO2..GPIO17
#define GB_A15_PIN      17
#define GB_DATA_OE_PIN  22   // 74LVC4245 level shifter U5 output enable
#define GB_D0_PIN       23   // D0..D7 = GPIO23..GPIO30 (3V side of U5)
#define GB_D7_PIN       30
#define GB_RESET_PIN    31   // assert (active high) drives /GB_RESET low via Q1

// NOTE on encoding `wait gpio`: rp2pio_statemachine_construct loads the program
// via the pico-SDK `pio_add_program_at_offset()` helper.  With
// PICO_PIO_USE_GPIO_BASE defined (true on RP2350B with 48 GPIOs) the SDK XORs
// each `wait gpio <n>` instruction's encoded 5-bit index with the PIO instance's
// GPIOBASE register (0 or 16) so that you encode the RAW ABSOLUTE GPIO number,
// and the hardware's own GPIOBASE pre-add then lands on the real pin.  This
// state machine lands on GPIO base 0 (all of /RD, A15 and D0..D7 are below
// GPIO32), so the rebase is a no-op and we encode /RD with its raw absolute
// number (GPIO20).  `jmp pin` (A15) and `out pins` (D0..D7) bases are likewise
// given as absolute pin numbers via sm_config_set_jmp_pin /
// sm_config_set_out_pins, which store both the low 5 bits and the high bits for
// the GPIOBASE-aware hardware -- exactly like the SM_PERIPHERAL helpers.

// The level-shifter output-enable (DATA_OE, GPIO22) is a SIDESET output of the
// data SM, not a static GPIO: the SM only asserts it (driving D0..D7 onto the
// GB bus) during ROM reads and the idle spin, and de-asserts it (high-Z) the
// rest of the time -- outside the ROM region (A15 high) and while the GB is
// held in reset -- so the RP2350 does not permanently drive the GB data bus.
// GB_DATA_OE_ASSERTED is the pin level that ENABLES the shifter output (the
// 74LVC4245 /OE is active low, so 0).  The SM's sideset encodes the RAW pin
// value, so a side of 0 enables the buffer and a side of 1 disables it.
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

static rp2pio_statemachine_obj_t data_sm;   // watches /RD + A15, drives D0..D7

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

// Side-set encodings for the DATA_OE pin.  Optional sideset (sideset_enable =
// true, 1 data bit): bit 12 of the instruction is the "this instr carries a
// side" flag, bit 11 is the driven OE pin value.  The 74LVC4245 /OE is active
// low, so pin 0 = buffer ON (driving D0..D7), pin 1 = buffer OFF (high-Z).
// OE_SIDE_NONE leaves DATA_OE unchanged so an instruction can be reached from
// both the "buffer on" and "buffer off" paths without a fixed side value.
#define OE_SIDE_NONE    0x0000
#define OE_SIDE_ENABLE  0x1000   // DATA_OE pin = 0 -> buffer drives D0..D7
#define OE_SIDE_DISABLE 0x1800   // DATA_OE pin = 1 -> buffer high-Z

#define DATA_PROG_LEN 8
static uint16_t data_program[DATA_PROG_LEN];

// The main loop wraps instruction 1 (out) .. instruction 5 (pull).  Instruction
// 0 is a one-shot prologue (prefetch byte[0] at SM entry with the buffer off).
// Instructions 6..7 are the A15-high "disabled spin", reached only via the
// explicit `jmp pin 6` at index 2 (never by falling through the wrap).  See
// build_pio_program().
#define DATA_WRAP_TARGET 1
#define DATA_WRAP        5

static void build_pio_program(void) {
    // gbio_data, single state machine.  out pins = D0..D7 (GPIO23..GPIO30).
    // jmp pin = A15 (GPIO17).  wait gpio = /RD (GPIO20).  sideset = DATA_OE
    // (GPIO22), 1 bit, optional.  Indices:
    //
    //   0: pull block             side=disable  ; (entry prologue) prefetch byte[0],
    //                                            ;  buffer off
    //   1: out pins, 8            side=none      ; (WRAP TARGET) drive current byte
    //                                            ;  onto D0..D7; OE left unchanged
    //   2: jmp pin 6              side=none      ; if A15 high (non-ROM) -> spin @6
    //   3: wait 0 gpio /RD        side=enable    ; A15 low: turn buffer ON, then wait
    //                                            ;  for /RD fall (the read)
    //   4: wait 1 gpio /RD        side=none      ; wait /RD rise (read done); OE on
    //   5: pull block             side=none      ; prefetch next byte; stalls here
    //                                            ;  when FIFO dry -> pins hold last
    //                                            ;  byte, OE stays on (idle spin)
    //   (wrap -> 1)                              ; drive freshly-pulled byte; OE on
    //   6: jmp pin 6              side=disable  ; A15-high spin: buffer OFF, loop
    //                                            ;  here while A15 high
    //   7: jmp 3                  side=none      ; A15 went low: go enable the buffer
    //                                            ;  and wait for the next /RD
    //
    // Drive-before-read alignment (unchanged from the prior single-SM design):
    // byte[0] is on D0..D7 before reset release; the buffer turns on at index 3,
    // which we reach when A15 goes low -- and A15 always goes low before /RD
    // falls (address setup), so OE is asserted in time and the game boy samples a
    // stable byte.  Exactly one byte is served per ROM read; the A15-high spin
    // (6/7) never executes `pull` or `out`, so the stream position is preserved
    // across non-ROM gaps and no extra pipeline delay is introduced.  read#N
    // still serves byte[N-1], matching the SAMD port (see the alignment note in
    // common_hal_gbio_reset_gameboy()).
    //
    // The DATA_OE sideset keeps the 74LVC4245 from permanently driving the bus:
    // the buffer is on only during the ROM region (A15 low, including the idle
    // spin at 0x1300) and off during the upper address half (A15 high) and during
    // reset (the entry prologue side=disable, before the first read).  When the
    // FIFO drains the SM stalls on the `pull` at index 5 (OE on), so the idle
    // 0xE9 stays readable -- which is exactly when the GB is replaying JP (HL) at
    // A15-low 0x1300.
    data_program[0] = pio_encode_pull(false, true) | OE_SIDE_DISABLE;
    data_program[1] = pio_encode_out(pio_pins, 8);
    data_program[2] = pio_encode_jmp_pin(6);
    data_program[3] = pio_encode_wait_gpio(false, GB_RD_PIN) | OE_SIDE_ENABLE;
    data_program[4] = pio_encode_wait_gpio(true, GB_RD_PIN);
    data_program[5] = pio_encode_pull(false, true);
    // wrap -> instruction 1 (out pins, 8) drives the freshly pulled byte.
    data_program[6] = pio_encode_jmp_pin(6) | OE_SIDE_DISABLE;
    data_program[7] = pio_encode_jmp(3);
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
    build_pio_program();

    // Address bus A0..A14 as plain SIO inputs (A15 is owned by the data SM as
    // its jmp pin).  sio_hw->gpio_in reads them regardless of pin function.
    for (uint8_t p = GB_A0_PIN; p < GB_A15_PIN; p++) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
        gpio_set_pulls(p, false, false);
    }
    // /RD as an input with a falling-edge interrupt handler registered (armed
    // only on demand).  It is also watched by the data SM via `wait gpio`, but
    // pad-level interrupt detection works independently of pin function.
    gpio_init(GB_RD_PIN);
    gpio_set_dir(GB_RD_PIN, GPIO_IN);
    gpio_set_pulls(GB_RD_PIN, false, false);
    gpio_add_raw_irq_handler(GB_RD_PIN, gbio_rd_dispatch);

    // /GB_RESET (active high to assert) is a static GPIO output.
    gpio_init(GB_RESET_PIN);
    gpio_set_dir(GB_RESET_PIN, GPIO_OUT);
    gpio_put(GB_RESET_PIN, 0);            // de-assert reset (GB running)
    // The level-shifter /OE is owned by the data SM as a sideset output (it is
    // only asserted during ROM reads / idle -- see build_pio_program).  Drive it
    // de-asserted (high = buffer OFF) here as a plain GPIO so the buffer stays
    // safely off until the SM is constructed and takes the pin over.
    gpio_init(GB_DATA_OE_PIN);
    gpio_set_dir(GB_DATA_OE_PIN, GPIO_OUT);
    gpio_put(GB_DATA_OE_PIN, 1);

    // --- data SM (GPIO base 0): watches /RD + A15 and drives D0..D7 from its
    //     TX FIFO.  All of these pins are below GPIO32, so one PIO state machine
    //     suffices. ---
    const mcu_pin_obj_t *a15_pin = mcu_get_pin_by_number(GB_A15_PIN);
    const mcu_pin_obj_t *d0_pin = mcu_get_pin_by_number(GB_D0_PIN);
    const mcu_pin_obj_t *oe_pin = mcu_get_pin_by_number(GB_DATA_OE_PIN);

    pio_pinmask_t data_pins = PIO_PINMASK_NONE;
    PIO_PINMASK_SET(data_pins, GB_A15_PIN);     // jmp pin (read by `jmp pin`)
    PIO_PINMASK_SET(data_pins, GB_RD_PIN);       // read by `wait gpio`
    for (uint8_t p = GB_D0_PIN; p <= GB_D7_PIN; p++) {
        PIO_PINMASK_SET(data_pins, p);           // out pins (driven by `out pins`)
    }
    PIO_PINMASK_SET(data_pins, GB_DATA_OE_PIN);  // sideset (driven by side bits)
    // /RD and A15 are PIO inputs; D0..D7 and DATA_OE are outputs.  DATA_OE
    // starts de-asserted (the 74LVC4245 /OE is active low, so pin high = high-Z)
    // so the buffer is off until the SM turns it on for a ROM read.
    pio_pinmask_t initial_dir = PIO_PINMASK_FROM_VALUE(((uint64_t)0xff) << GB_D0_PIN);
    PIO_PINMASK_SET(initial_dir, GB_DATA_OE_PIN);
    pio_pinmask_t initial_state = PIO_PINMASK_FROM_PIN(GB_DATA_OE_PIN);

    bool ok = rp2pio_statemachine_construct(&data_sm,
        data_program, DATA_PROG_LEN,
        0 /* frequency: use clk_sys */,
        NULL, 0,                                   // init
        d0_pin, 8,                                 // out pins = D0..D7
        NULL, 0,                                   // in
        PIO_PINMASK_NONE, PIO_PINMASK_NONE,        // pull up/down
        NULL, 0,                                   // set
        oe_pin, 1, false,                          // sideset = DATA_OE (values, not pindirs)
        initial_state,                             // initial pin state (DATA_OE high)
        initial_dir,                               // initial pin dirs (D0..D7 + OE out)
        a15_pin,                                   // jmp pin = A15
        data_pins,
        true, false,                              // tx fifo yes, rx no
        false, 8, true,                           // manual pull (no auto_pull); shift right (LSB->D0)
        false,                                    // wait_for_txstall
        false, 0, false,                          // auto_push
        true,                                     // claim pins
        false,                                     // not user interruptible
        true,                                      // sideset_enable (optional sideset)
        DATA_WRAP_TARGET, DATA_WRAP, PIO_ANY_OFFSET, // wrap target=1, wrap=5
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
    // Alignment with the SAMD gb_m4 port (the boot streams are byte-identical,
    // so the served-byte-per-cart-address mapping must match SAMD exactly):
    //
    //   * There is NO one-read pipeline delay.  The very first response byte
    //     is gameboy_boot[0] (read#1), and read#N serves gameboy_boot[N-1].
    //   * Proof from the SAMD GBC-detection check `address_i == 50 && address
    //     == 0x0104`: on a DMG the 50th read must NOT be at 0x0104 (else the
    //     DMG would be falsely detected as a GBC and re-armed with the wrong
    //     stream).  The DMG reads the logo at 0x0104 at read#K != 50, and 0x0104
    //     must receive gameboy_boot[48] = 0xCE (start of the "Nintendo logo 48
    //     bytes" chunk, which the DMG boot ROM compares against its internal
    //     Nintendo logo).  read#N -> gameboy_boot[N-1] gives K = 49 (and GBC,
    //     which reads 0x0104 one read later, lands on read#50 -> detected).
    //     read#N -> gameboy_boot[N-2] (a one-read delay) would force K = 50 --
    //     a false-positive GBC detection AND a corrupt logo -- so we must NOT
    //     prepend a placeholder byte here.
    //
    // The data SM (build_pio_program) drives each byte onto D0..D7 BEFORE
    // its /RD strobe, so it serves gameboy_boot[0] stably on read#1 -- which
    // is exactly how the SAMD port sets its first response byte (loaded at
    // reset release / the previous read's rising edge, stable for the read).
    mp_printf(&mp_plat_print, "  [gbio] stage 3: starting DMG feeder (gameboy_boot)\n");
    feeder_start(gameboy_boot, sizeof(gameboy_boot));

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
