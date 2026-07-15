// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// game boy cartridge interface for the PyGameBoy RP2350.
//
// The RP2350 plays the role of a game boy cartridge.  A 64K buffer in RAM
// maps the entire Game Boy address space.  PIO state machines and DMA
// automatically serve reads from this buffer and capture writes into it,
// leaving the ARM core free to run Python / USB between Game Boy accesses.
//
// Architecture:
//
//   1. Two monitor SMs watch /CS (GPIO21) and A15 (GPIO17).  They share a
//      program but use different jmp pins.  When their pin goes low they
//      clear a shared PIO IRQ; when it goes high they set it again.
//
//   2. An address SM waits for both IRQs to be cleared, then captures the
//      full 16-bit address from A0..A15 into its RX FIFO.
//
//   3. A DMA channel copies the address from the RX FIFO into a circular
//      buffer in memory.  The DMA sniffer is set to monitor this channel in
//      sum mode.  Its initial value is the pointer to the 64K data buffer,
//      so after the address transfer the sniffer holds buffer_ptr + address.
//
//   4. The address DMA chains to a DMA that copies the sniffer value into
//      the READ DMA's source-address register, causing a read from the
//      correct offset in the 64K buffer.  That chains to a DMA that resets
//      the sniffer back to the buffer base pointer.
//
//   5. The output SM waits for the shared IRQs, then waits for clock low.
//      On a Game Boy write (/WR low) it reads 8 bits from the bus into its
//      RX FIFO and discards 8 bits from its TX FIFO.  On a Game Boy read
//      (/WR high) it outputs 8 bits from its TX FIFO.  It then waits for
//      clock high, disables OE, and wraps.
//
// The 64K buffer is the primary interface: the ARM core writes SM83 opcodes
// into it at the addresses the Game Boy will read, and the PIO/DMA hardware
// serves them automatically.  The command-stream API (queue_commands, etc.)
// is preserved for compatibility, implemented by writing into the buffer.

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
#define GB_A0_PIN       2    // A0..A15 = GPIO2..GPIO17
#define GB_A15_PIN      17
#define GB_CLK_PIN      18
#define GB_WR_PIN       19
#define GB_RD_PIN       20   // /RD  (input from GB)
#define GB_CS_PIN       21
#define GB_DATA_OE_PIN  22   // 74LVC4245 level shifter U5 output enable
#define GB_D0_PIN       23   // D0..D7 = GPIO23..GPIO30 (3V side of U5)
#define GB_D7_PIN       30
#define GB_RESET_PIN    31   // assert (active high) drives /GB_RESET low via Q1

// ===== PIO IRQ ASSIGNMENTS =====
// IRQ 0: cleared by CS monitor SM when /CS goes low
// IRQ 1: cleared by A15 monitor SM when A15 goes low
// The address SM and output SM both wait for these IRQs to be cleared.
#define IRQ_CS   0
#define IRQ_A15  1

// ===== 64K DATA BUFFER =====
// Maps the entire Game Boy address space.  The ARM core writes SM83 opcodes
// here; the PIO/DMA hardware serves them automatically on every access.
static uint8_t gb_data_buffer[65536] __attribute__((aligned(4)));

// ===== CIRCULAR ADDRESS BUFFER (for debugging) =====
#define ADDRESS_BUFFER_SIZE 1024  // must be power of 2
static uint16_t gb_address_buffer[ADDRESS_BUFFER_SIZE] __attribute__((aligned(4)));

// ===== DEBUG PIO =====
// A second PIO state machine samples all GPIO0..GPIO31 on every falling edge
// of /RD (GPIO20) and pushes 32-bit snapshots into its RX FIFO.  A dedicated
// DMA channel transfers the first N samples into a RAM buffer for post-mortem
// analysis after reset_gameboy().  We trigger on /RD because it is the one
// signal we know is toggling (the data SM already watches it).
#define DEBUG_SAMPLE_COUNT (114560 / 2)

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

// ===== SM83 COMMAND STREAMS (flat-buffer layout) =====
//
// With the 64K flat buffer, each byte is placed at its actual Game Boy
// address.  The boot sequence works in two phases:
//   1. Adafruit logo at 0x0104 – the boot ROM reads it for the scrolling
//      animation.
//   2. After ~500 ms we overwrite 0x0104..0x0133 with the real Nintendo
//      logo so that the boot ROM's verification pass succeeds.

// Adafruit logo (48 bytes) – shown during the boot scroll animation.
static const uint8_t adafruit_logo[48] = {
    0x00, 0x30, 0x00, 0xC6, 0x00, 0x07, 0xCC, 0xCC, 0x00, 0xF1, 0x13, 0x3B, 0xC0, 0xD1, 0x00, 0xBD,
    0x00, 0x66, 0x00, 0x66, 0xC1, 0xDD, 0x08, 0xE8, 0x36, 0x63, 0xE6, 0xE6, 0xCC, 0xC7, 0xCD, 0xDC,
    0xF9, 0xBD, 0xBB, 0xBB, 0x11, 0x11, 0x88, 0x88, 0x66, 0x63, 0x66, 0xE6, 0xDD, 0xDD, 0x88, 0x8E,
};

// Nintendo logo (48 bytes) – required for the boot ROM verification pass.
static const uint8_t nintendo_logo[48] = {
    0xce, 0xed, 0x66, 0x66, 0xcc, 0x0d, 0x00, 0x0b, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0c, 0x00, 0x0d,
    0x00, 0x08, 0x11, 0x1f, 0x88, 0x89, 0x00, 0x0e, 0xdc, 0xcc, 0x6e, 0xe6, 0xdd, 0xdd, 0xd9, 0x99,
    0xbb, 0xbb, 0x67, 0x63, 0x6e, 0x0e, 0xec, 0xcc, 0xdd, 0xdc, 0x99, 0x9f, 0xbb, 0xb9, 0x33, 0x3e,
};

// Cartridge header (0x0134..0x014F, 28 bytes).
// Header checksum at 0x014D is computed so that sum(0x0134..0x014D) == 0.
static const uint8_t cartridge_header[28] = {
    'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd',  // title (11)
    0x00, 0x00, 0x00, 0x00,                                   // title padding to 15
    0x00,                                                      // GBC flag: DMG only
    0x00, 0x00,                                                // new licensee code
    0x00,                                                      // SGB flag
    0x01,                                                      // cartridge type: MBC1
    0x00,                                                      // ROM size: 32 KB
    0x00,                                                      // RAM size: none
    0x00,                                                      // destination: Japanese
    0xCA,                                                      // old licensee code
    0x31,                                                      // mask ROM version
    0xE8,                                                      // header checksum (computed)
    0x00, 0x00,                                                // global checksum (unused)
};

// Boot code placed at 0x0150 (after the cartridge header).
// Sets up the stack pointer, joypad, interrupts, then spins on JP (HL)
// at 0x1000 waiting for commands from the ARM core.
static const uint8_t boot_code[] = {
    // Set the stack pointer
    0x31, 0x00, 0xe0,                             // LD SP, 0xE000

    0x00, 0x00,

    // Set both lines low to detect any press.
    0xf0, 0x00,                                   // LD A, (0xFF00) – poke GPIO
    0x3e, 0x00,                                   // LD A, 0x30
    0xe0, 0x00,                                   // LD (0xFF00), A

    0x00, 0x00,

    // Clear vsync and joypad interrupts
    0x3e, 0x00,                                   // LD A, 0x00
    0xe0, 0x0f,                                   // LD (0xFF0F), A

    // Enable vsync and joypad interrupts
    0x3e, 0x11,                                   // LD A, 0x11 (vblank is bit 0)
    0xe0, 0xff,                                   // LD (0xFFFF), A

    0x00, 0x00,

    0xfb,                                         // EI
    0xfb,                                         // EI
    0xfb,                                         // EI

    0x00, 0x00,

    // Load 0x1000 into hl and repeatedly jump to it until we do
    // something else. This prevents the program counter from
    // exiting the cartridge address range.
    0x21, 0x00, 0x10, 0xe9                        // LD HL, 0x1000; JP (HL)
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

// Idle address the game boy spins on between command sequences.
#define GB_IDLE_ADDR 0x1000

// PIO instances and state machine indices (raw Pico SDK, not CircuitPython wrapper)
static PIO gb_pio0;  // monitors + address
static PIO gb_pio1;  // output
static int monitor_cs_sm;
static int monitor_a15_sm;
static int address_sm;
static int output_sm;

// Program offsets in PIO instruction memory
static uint monitor_prog_offset;
static uint address_prog_offset;
static uint output_prog_offset;

// DMA channels for the sniffer chain
static int dma_addr_chan = -1;         // address SM RX FIFO -> circular buffer
static int dma_sniff_read_chan = -1;   // sniffer -> data read DMA read addr
static int dma_sniff_write_chan = -1;  // sniffer -> data write DMA write addr
static int dma_sniff_reset_chan = -1;  // reset sniffer to buffer base
static int dma_data_read_chan = -1;    // data buffer -> output SM TX FIFO
static int dma_data_write_chan = -1;   // output SM RX FIFO -> data buffer

// Cached base pointer for sniffer reset
static uint32_t gb_buffer_base;

// Legacy dma_chan for feeder compatibility (aliases dma_data_read_chan)
#define dma_chan dma_data_read_chan

// Command buffers (in ordinary RAM; the SAMD port used backup RAM but the
// RP2350 keeps these in normal SRAM and re-arms them on reset_gameboy).
static uint8_t command_cache[1024] __attribute__((aligned(4)));

static volatile uint32_t vsync_count = 0;
static volatile uint64_t last_vsync_time = 0;

static volatile uint32_t total_additional_cycles;

static volatile bool everything_going = false;     // boot complete?
static volatile bool gameboy_color_booting = false;
static volatile bool gameboy_color = false;

// ===== FIXED INTERRUPT HANDLERS IN 64K BUFFER =====
// These are copied into the 64K buffer at init/reset time.
// The Game Boy interrupt vectors (0x0040, 0x0060) contain JP instructions
// to these handlers.

// Cartridge RAM base address (GB can write here, ARM can read from buffer)
#define GB_RAM_BASE 0xA000
#define GB_RAM_VSYNC (GB_RAM_BASE + 0)  // byte: frame counter
#define GB_RAM_GAMEPAD (GB_RAM_BASE + 1) // byte: button state

// VBlank handler address in buffer
#define VB_HANDLER_ADDR 0x0200
// Joypad handler address in buffer
#define JP_HANDLER_ADDR 0x0280

// VBlank handler: increments frame counter in cartridge RAM, runs user commands, returns.
// The ARM writes user commands into the user command area (VB_HANDLER_ADDR + prologue size).
#define VB_USER_AREA (VB_HANDLER_ADDR + 24)  // offset past prologue
#define VB_USER_AREA_SIZE 128

static uint8_t vblank_handler_prologue[] = {
    // PUSH AF, PUSH HL
    0xF5, 0xE5,
    // Increment frame counter at GB_RAM_VSYNC
    0x21, (uint8_t)(GB_RAM_VSYNC & 0xFF), (uint8_t)(GB_RAM_VSYNC >> 8), // LD HL, GB_RAM_VSYNC
    0x7E,                         // LD A, (HL)
    0x3C,                         // INC A
    0x77,                         // LD (HL), A
    // Clear VBlank interrupt flag
    0x3E, 0xEF,                   // LD A, 0xEF
    0xE0, 0x0F,                   // LDH (0xFF0F), A
    // POP HL, POP AF
    0xE1, 0xF1,
    0xD9,                         // RETI
    // User command area follows (filled with NOPs by default)
};

// Joypad handler: reads joypad register, writes button state to cartridge RAM.
static uint8_t joypad_handler[] = {
    // PUSH AF, PUSH BC
    0xF5, 0xC5,
    // Select directions (P14=0, P15=1): write 0x20 to 0xFF00
    0x3E, 0x20,                   // LD A, 0x20
    0xE0, 0x00,                   // LDH (0xFF00), A
    0x00,                         // NOP (timing)
    0xF0, 0x00,                   // LDH A, (0xFF00)
    0xE6, 0x0F,                   // AND 0x0F
    0x47,                         // LD B, A
    // Select buttons (P14=1, P15=0): write 0x10 to 0xFF00
    0x3E, 0x10,                   // LD A, 0x10
    0xE0, 0x00,                   // LDH (0xFF00), A
    0x00,                         // NOP (timing)
    0xF0, 0x00,                   // LDH A, (0xFF00)
    0xE6, 0x0F,                   // AND 0x0F
    0xCB, 0x37,                   // SWAP A
    0xB0,                         // OR B
    // Write to cartridge RAM at GB_RAM_GAMEPAD
    0x21, (uint8_t)(GB_RAM_GAMEPAD & 0xFF), (uint8_t)(GB_RAM_GAMEPAD >> 8), // LD HL, GB_RAM_GAMEPAD
    0x77,                         // LD (HL), A
    // Reset joypad (both columns high)
    0x3E, 0x30,                   // LD A, 0x30
    0xE0, 0x00,                   // LDH (0xFF00), A
    // Clear joypad interrupt flag
    0x3E, 0xEF,                   // LD A, 0xEF
    0xE0, 0x0F,                   // LDH (0xFF0F), A
    // POP BC, POP AF
    0xC1, 0xF1,
    0xD9,                         // RETI
};

// ===== PIO PROGRAMS =====
//
// Built at runtime with the pio_encode_* helpers.

// Side-set encodings for the DATA_OE pin.  Optional sideset (sideset_enable =
// true, 1 data bit): bit 12 of the instruction is the "this instr carries a
// side" flag, bit 11 is the driven OE pin value.  The 74LVC4245 /OE is active
// low, so pin 0 = buffer ON (driving D0..D7), pin 1 = buffer OFF (high-Z).
#define OE_SIDE_NONE    0x0000
#define OE_SIDE_ENABLE  0x1000   // DATA_OE pin = 0 -> buffer drives D0..D7
#define OE_SIDE_DISABLE 0x1800   // DATA_OE pin = 1 -> buffer high-Z

// ---- Monitor program (shared by CS and A15 SMs) ----
// Uses jmp pin to monitor the assigned signal.
// When jmp pin goes low: clear IRQ (signal "access starting")
// When jmp pin goes high: set IRQ (signal "access complete")
#define MONITOR_PROG_LEN 5
static uint16_t monitor_program[MONITOR_PROG_LEN];

static void build_monitor_program(void) {
    // Instruction 0: jmp pin 0  -- loop while jmp pin is high
    // Instruction 1: irq clear N -- signal address/output SMs (IRQ patched at init)
    // Instruction 2: jmp pin 4  -- if jmp pin high, jump to done
    // Instruction 3: jmp 2      -- loop while jmp pin is low
    // Instruction 4: irq set N   -- clear signal, wrap to 0
    monitor_program[0] = pio_encode_jmp_pin(0);
    monitor_program[1] = pio_encode_irq_clear(false, 0);  // IRQ patched
    monitor_program[2] = pio_encode_jmp_pin(4);
    monitor_program[3] = pio_encode_jmp(2);
    monitor_program[4] = pio_encode_irq_set(false, 0);    // IRQ patched
}

static void patch_monitor_irq(uint16_t *prog, uint irq_num) {
    prog[1] = pio_encode_irq_clear(false, irq_num);
    prog[4] = pio_encode_irq_set(false, irq_num);
}

// ---- Address capture program ----
// Waits for both IRQs to be cleared (CS low AND A15 low),
// then captures the 16-bit address from A0..A15 into RX FIFO.
#define ADDRESS_PROG_LEN 4
static uint16_t address_program[ADDRESS_PROG_LEN];

static void build_address_program(void) {
    // Instruction 0: wait 0 irq 0  -- wait for CS low
    // Instruction 1: wait 0 irq 1  -- wait for A15 low
    // Instruction 2: in pins, 16   -- capture address
    // Instruction 3: push          -- push to RX FIFO
    address_program[0] = pio_encode_wait_irq(false, false, IRQ_CS);
    address_program[1] = pio_encode_wait_irq(false, false, IRQ_A15);
    address_program[2] = pio_encode_in(pio_pins, 16);
    address_program[3] = pio_encode_push(false, false);
}

// ---- Output program ----
// Handles both reads and writes on the Game Boy bus.
// Jmp pin = /WR (GPIO19): high = read, low = write
// Out pins = D0..D7 (GPIO23..30), In pins = D0..D7
// Sideset = DATA_OE (GPIO22)
#define OUTPUT_PROG_LEN 12
static uint16_t output_program[OUTPUT_PROG_LEN];

static void build_output_program(void) {
    // Instruction 0:  wait 0 irq 0        -- wait for CS low
    // Instruction 1:  wait 0 irq 1        -- wait for A15 low
    // Instruction 2:  wait 0 gpio CLK     -- wait for clock low
    // Instruction 3:  jmp pin 8           -- if /WR high, it's a read
    // Instruction 4:  in pins, 8          -- [write] read data from bus
    // Instruction 5:  push                -- push to RX FIFO
    // Instruction 6:  pull                -- discard from TX FIFO
    // Instruction 7:  jmp 10              -- jump to done
    // Instruction 8:  pull                -- [read] get byte from TX FIFO
    // Instruction 9:  out pins, 8         -- output to bus + OE enabled
    // Instruction 10: wait 1 gpio CLK     -- wait for clock high
    // Instruction 11: nop + OE disable    -- disable OE, wrap to 0
    output_program[0] = pio_encode_wait_irq(false, false, IRQ_CS) | OE_SIDE_DISABLE;
    output_program[1] = pio_encode_wait_irq(false, false, IRQ_A15) | OE_SIDE_DISABLE;
    output_program[2] = pio_encode_wait_gpio(false, GB_CLK_PIN) | OE_SIDE_DISABLE;
    output_program[3] = pio_encode_jmp_pin(8);
    // Write path
    output_program[4] = pio_encode_in(pio_pins, 8) | OE_SIDE_DISABLE;
    output_program[5] = pio_encode_push(false, false) | OE_SIDE_DISABLE;
    output_program[6] = pio_encode_pull(false, false) | OE_SIDE_DISABLE;
    output_program[7] = pio_encode_jmp(10);
    // Read path
    output_program[8] = pio_encode_pull(false, false) | OE_SIDE_DISABLE;
    output_program[9] = pio_encode_out(pio_pins, 8) | OE_SIDE_ENABLE;
    // Done
    output_program[10] = pio_encode_wait_gpio(true, GB_CLK_PIN) | OE_SIDE_ENABLE;
    output_program[11] = pio_encode_nop() | OE_SIDE_DISABLE;
}

// ===== DEBUG PIO =====
// A second PIO state machine samples all GPIO0..GPIO31 on every falling edge
// of /RD (GPIO20) and pushes 32-bit snapshots into its RX FIFO.  A dedicated
// DMA channel transfers the first N samples into a RAM buffer for post-mortem
// analysis after reset_gameboy().  We trigger on /RD because it is the one
// signal we know is toggling (the data SM already watches it).
#define DEBUG_SAMPLE_COUNT (114560 / 2)

#define DEBUG_PROG_LEN 5
static uint16_t debug_program[DEBUG_PROG_LEN];

static void build_debug_program(void) {
    // Capture all 32 low GPIO bits (GPIO0..GPIO31) on every /RD falling edge.
    // No A15 gating — capture all bus activity (ROM, I/O, everything).
    // in_base = GPIO0, autopush at 32 bits.
    //
    // Bit layout in each 32-bit sample:
    //   bits 0..1:   GPIO0..GPIO1
    //   bits 2..17:  A0..A15 (GPIO2..GPIO17)
    //   bit 18:      (GPIO18)
    //   bit 19:      (GPIO19)
    //   bit 20:      /RD (GPIO20)
    //   bit 21:      (GPIO21)
    //   bit 22:      DATA_OE (GPIO22)
    //   bits 23..30: D0..D7 (GPIO23..GPIO30)
    //   bit 31:      /GB_RESET (GPIO31)
    debug_program[0] = pio_encode_wait_gpio(false, GB_A15_PIN);  // wait for A15 falling edge to indicate ROM access
    debug_program[1] = pio_encode_in(pio_pins, 32);             // sample address (and everything else)
    debug_program[2] = pio_encode_wait_gpio(false, GB_CLK_PIN);   // wait for clock low because the data is read in the middle of the low pulse
    debug_program[3] = pio_encode_in(pio_pins, 32);             // sample data we're responding with
    debug_program[4] = pio_encode_wait_gpio(true, GB_A15_PIN);  // wait for A15 to rise again to complete the cycle
    // wraps automatically from instruction 2 back to 0
}

// ===== DMA / SNIFFER CHAIN =====

static void setup_dma_chain(void) {
    dma_addr_chan = (int)dma_claim_unused_channel(false);
    dma_sniff_read_chan = (int)dma_claim_unused_channel(false);
    dma_sniff_write_chan = (int)dma_claim_unused_channel(false);
    dma_sniff_reset_chan = (int)dma_claim_unused_channel(false);
    dma_data_read_chan = (int)dma_claim_unused_channel(false);
    dma_data_write_chan = (int)dma_claim_unused_channel(false);

    if (dma_addr_chan < 0 || dma_sniff_read_chan < 0 ||
        dma_sniff_write_chan < 0 || dma_sniff_reset_chan < 0 ||
        dma_data_read_chan < 0 || dma_data_write_chan < 0) {
        mp_printf(&mp_plat_print, "gbio: failed to claim DMA channels\n");
        return;
    }

    mp_printf(&mp_plat_print, "gbio: DMA channels: addr=%d sniff_read=%d sniff_write=%d sniff_reset=%d data_read=%d data_write=%d\n",
        dma_addr_chan, dma_sniff_read_chan, dma_sniff_write_chan,
        dma_sniff_reset_chan, dma_data_read_chan, dma_data_write_chan);

    gb_buffer_base = (uint32_t)gb_data_buffer;

    // ---- DMA_CHAN_ADDR: address SM RX FIFO -> circular buffer ----
    // This channel is watched by the sniffer in sum mode.
    {
        dma_channel_config c = dma_channel_get_default_config(dma_addr_chan);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_ring(&c, true, 10);  // wrap write at 2^10 = 1024 entries
        channel_config_set_dreq(&c, pio_get_dreq(gb_pio0, address_sm, false));
        channel_config_set_chain_to(&c, dma_sniff_read_chan);
        channel_config_set_sniff_enable(&c, true);
        dma_channel_configure(dma_addr_chan, &c,
            gb_address_buffer,
            &gb_pio0->rxf[address_sm],
            1, true);
    }

    // Configure DMA sniffer: sum mode, initial value = buffer base
    dma_sniffer_enable(dma_addr_chan, DMA_SNIFF_CTRL_CALC_VALUE_SUM, true);
    dma_sniffer_set_data_accumulator(gb_buffer_base);

    // ---- DMA_CHAN_SNIFF_READ: sniffer -> data_read DMA read address ----
    {
        dma_channel_config c = dma_channel_get_default_config(dma_sniff_read_chan);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, false);
        channel_config_set_chain_to(&c, dma_sniff_write_chan);
        dma_channel_configure(dma_sniff_read_chan, &c,
            &dma_channel_hw_addr(dma_data_read_chan)->al3_read_addr_trig,
            &dma_hw->sniff_data,
            1, false);
    }

    // ---- DMA_CHAN_DATA_READ: data buffer -> output SM TX FIFO ----
    {
        dma_channel_config c = dma_channel_get_default_config(dma_data_read_chan);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(gb_pio1, output_sm - NUM_PIO_STATE_MACHINES, true));
        // channel_config_set_chain_to(&c, dma_sniff_write_chan);
        dma_channel_configure(dma_data_read_chan, &c,
            &gb_pio1->txf[output_sm - NUM_PIO_STATE_MACHINES],
            gb_data_buffer,
            1, false);
    }

    // ---- DMA_CHAN_SNIFF_WRITE: sniffer -> data_write DMA write address ----
    {
        dma_channel_config c = dma_channel_get_default_config(dma_sniff_write_chan);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, false);
        channel_config_set_chain_to(&c, dma_sniff_reset_chan);
        dma_channel_configure(dma_sniff_write_chan, &c,
            &dma_channel_hw_addr(dma_data_write_chan)->al2_write_addr_trig,
            &dma_hw->sniff_data,
            1, false);
    }

    // ---- DMA_CHAN_SNIFF_RESET: reset sniffer to buffer base ----
    {
        dma_channel_config c = dma_channel_get_default_config(dma_sniff_reset_chan);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, false);
        channel_config_set_chain_to(&c, dma_addr_chan);
        dma_channel_configure(dma_sniff_reset_chan, &c,
            &dma_hw->sniff_data,
            &gb_buffer_base,
            1, false);
    }

    // ---- DMA_CHAN_DATA_WRITE: output SM RX FIFO -> data buffer ----
    // Independent channel, not in the main chain.  Triggered by RX FIFO data.
    {
        dma_channel_config c = dma_channel_get_default_config(dma_data_write_chan);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(gb_pio1, output_sm - NUM_PIO_STATE_MACHINES, false));
        dma_channel_configure(dma_data_write_chan, &c,
            gb_data_buffer,
            &gb_pio1->rxf[output_sm - NUM_PIO_STATE_MACHINES],
            1, false);
    }

    mp_printf(&mp_plat_print, "gbio: DMA chain setup complete\n");
}

// ===== FEEDER HELPERS (for command-stream compatibility) =====

static void feeder_start(const uint8_t *buf, size_t len) {
    // Write the command bytes into the 64K buffer at GB_IDLE_ADDR.
    // The Game Boy is spinning on JP (HL) at GB_IDLE_ADDR, reading 0xE9.
    // We replace the idle bytes with the command sequence.
    mp_printf(&mp_plat_print, "  [gbio] feeder_start: len=%u at 0x%04X\n",
        (unsigned)len, GB_IDLE_ADDR);
    if (len <= sizeof(gb_data_buffer) - GB_IDLE_ADDR) {
        memcpy(gb_data_buffer + GB_IDLE_ADDR, buf, len);
    }
}

static void feeder_wait_drained(void) {
    // With the 64K buffer, there's no feeder to drain.
    // The PIO/DMA serves bytes directly from the buffer.
    // Give the Game Boy time to consume the commands.
    mp_printf(&mp_plat_print, "  [gbio] feeder_wait_drained: waiting\n");
    common_hal_mcu_delay_us(500);
}

// ===== INIT =====

// ===== DEBUG PIO STATE =====
static PIO debug_pio = NULL;
static int debug_sm = -1;
static uint debug_prog_offset;
static int debug_dma_chan = -1;
static bool debug_configured = false;
static uint32_t debug_samples[DEBUG_SAMPLE_COUNT] __attribute__((aligned(4)));

static bool gbio_inited = false;

void gbio_init(void) {
    if (gbio_inited) {
        return;
    }
    mp_printf(&mp_plat_print, "start gbio_init()\n");
    build_monitor_program();
    build_address_program();
    build_output_program();

    // Address bus A0..A15 as plain SIO inputs.  sio_hw->gpio_in reads them
    // regardless of pin function.
    for (uint8_t p = GB_A0_PIN; p <= GB_A15_PIN; p++) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
        gpio_set_pulls(p, false, false);
    }
    // /RD, /WR, /CS, CLK as inputs
    gpio_init(GB_RD_PIN);
    gpio_set_dir(GB_RD_PIN, GPIO_IN);
    gpio_set_pulls(GB_RD_PIN, false, false);

    gpio_init(GB_WR_PIN);
    gpio_set_dir(GB_WR_PIN, GPIO_IN);
    gpio_set_pulls(GB_WR_PIN, false, false);

    gpio_init(GB_CS_PIN);
    gpio_set_dir(GB_CS_PIN, GPIO_IN);
    gpio_set_pulls(GB_CS_PIN, false, false);

    gpio_init(GB_CLK_PIN);
    gpio_set_dir(GB_CLK_PIN, GPIO_IN);
    gpio_set_pulls(GB_CLK_PIN, false, false);

    // Configure any unclaimed pins between A15 and D0 as SIO inputs so the
    // debug PIO's `in pins, 29` (GPIO2..GPIO30) captures clean levels on every
    // pin rather than floating values.
    for (uint8_t p = GB_A15_PIN + 1; p < GB_D0_PIN; p++) {
        if (p == GB_RD_PIN || p == GB_WR_PIN || p == GB_CS_PIN ||
            p == GB_CLK_PIN || p == GB_DATA_OE_PIN) {
            continue;
        }
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
        gpio_set_pulls(p, false, false);
    }

    // /GB_RESET (active high to assert) is a static GPIO output.
    gpio_init(GB_RESET_PIN);
    gpio_set_dir(GB_RESET_PIN, GPIO_OUT);
    gpio_put(GB_RESET_PIN, 0);            // de-assert reset (GB running)
    // The level-shifter /OE is owned by the output SM as a sideset output.
    // Drive it de-asserted (high = buffer OFF) here as a plain GPIO so the
    // buffer stays safely off until the SM is constructed and takes the pin over.
    gpio_init(GB_DATA_OE_PIN);
    gpio_set_dir(GB_DATA_OE_PIN, GPIO_OUT);
    gpio_put(GB_DATA_OE_PIN, 1);

    // ---- Claim PIO state machines ----
    // We need 4 SMs: 2 monitors (share program), 1 address, 1 output
    // Use PIO0 for monitors and address (3 SMs), PIO1 for output (1 SM)
    gb_pio0 = pio0;
    gb_pio1 = pio1;

    int m_cs = pio_claim_unused_sm(gb_pio0, false);
    int m_a15 = pio_claim_unused_sm(gb_pio0, false);
    int a_sm = pio_claim_unused_sm(gb_pio0, false);
    int o_sm = pio_claim_unused_sm(gb_pio1, false);

    if (m_cs < 0 || m_a15 < 0 || a_sm < 0 || o_sm < 0) {
        mp_printf(&mp_plat_print, "gbio: failed to claim state machines\n");
        return;
    }
    monitor_cs_sm = m_cs;
    monitor_a15_sm = m_a15;
    address_sm = a_sm;
    output_sm = o_sm + NUM_PIO_STATE_MACHINES;  // offset for PIO1

    mp_printf(&mp_plat_print, "gbio: SMs - monitor_cs=%d monitor_a15=%d address=%d output=%d\n",
        monitor_cs_sm, monitor_a15_sm, address_sm, output_sm);

    // ---- Load programs into PIO instruction memory ----
    pio_program_t monitor_prog_info = {
        .instructions = monitor_program,
        .length = MONITOR_PROG_LEN,
        .origin = -1,
    };
    monitor_prog_offset = pio_add_program(gb_pio0, &monitor_prog_info);

    pio_program_t address_prog_info = {
        .instructions = address_program,
        .length = ADDRESS_PROG_LEN,
        .origin = -1,
    };
    address_prog_offset = pio_add_program(gb_pio0, &address_prog_info);

    pio_program_t output_prog_info = {
        .instructions = output_program,
        .length = OUTPUT_PROG_LEN,
        .origin = -1,
    };
    output_prog_offset = pio_add_program(gb_pio1, &output_prog_info);

    mp_printf(&mp_plat_print, "gbio: program offsets - monitor=%d address=%d output=%d\n",
        monitor_prog_offset, address_prog_offset, output_prog_offset);

    // ---- Configure monitor CS SM (jmp pin = /CS, IRQ 0) ----
    {
        uint16_t cs_prog[MONITOR_PROG_LEN];
        memcpy(cs_prog, monitor_program, sizeof(cs_prog));
        patch_monitor_irq(cs_prog, IRQ_CS);

        pio_sm_config cfg = pio_get_default_sm_config();
        sm_config_set_wrap(&cfg, monitor_prog_offset, monitor_prog_offset + MONITOR_PROG_LEN - 1);
        sm_config_set_jmp_pin(&cfg, GB_CS_PIN);
        pio_sm_init(gb_pio0, monitor_cs_sm, monitor_prog_offset, &cfg);
        for (uint i = 0; i < MONITOR_PROG_LEN; i++) {
            gb_pio0->instr_mem[monitor_prog_offset + i] = cs_prog[i];
        }
        pio_sm_set_enabled(gb_pio0, monitor_cs_sm, true);
    }

    // ---- Configure monitor A15 SM (jmp pin = A15, IRQ 1) ----
    {
        uint16_t a15_prog[MONITOR_PROG_LEN];
        memcpy(a15_prog, monitor_program, sizeof(a15_prog));
        patch_monitor_irq(a15_prog, IRQ_A15);

        pio_sm_config cfg = pio_get_default_sm_config();
        sm_config_set_wrap(&cfg, monitor_prog_offset, monitor_prog_offset + MONITOR_PROG_LEN - 1);
        sm_config_set_jmp_pin(&cfg, GB_A15_PIN);
        pio_sm_init(gb_pio0, monitor_a15_sm, monitor_prog_offset, &cfg);
        for (uint i = 0; i < MONITOR_PROG_LEN; i++) {
            gb_pio0->instr_mem[monitor_prog_offset + i] = a15_prog[i];
        }
        pio_sm_set_enabled(gb_pio0, monitor_a15_sm, true);
    }

    // ---- Configure address SM (in pins = A0..A15) ----
    {
        pio_sm_config cfg = pio_get_default_sm_config();
        sm_config_set_wrap(&cfg, address_prog_offset, address_prog_offset + ADDRESS_PROG_LEN - 1);
        sm_config_set_in_pins(&cfg, GB_A0_PIN);
        sm_config_set_in_shift(&cfg, false, true, 16);  // shift right, autopush at 16 bits
        pio_sm_init(gb_pio0, address_sm, address_prog_offset, &cfg);
        pio_sm_set_enabled(gb_pio0, address_sm, true);
    }

    // ---- Configure output SM (out/in pins = D0..D7, jmp pin = /WR, sideset = DATA_OE) ----
    {
        uint sm_idx = output_sm - NUM_PIO_STATE_MACHINES;
        pio_sm_config cfg = pio_get_default_sm_config();
        sm_config_set_wrap(&cfg, output_prog_offset, output_prog_offset + OUTPUT_PROG_LEN - 1);
        sm_config_set_out_pins(&cfg, GB_D0_PIN, 8);
        sm_config_set_in_pins(&cfg, GB_D0_PIN);
        sm_config_set_jmp_pin(&cfg, GB_WR_PIN);
        sm_config_set_sideset_pins(&cfg, GB_DATA_OE_PIN);
        sm_config_set_sideset(&cfg, 1, false, false);  // 1 bit, values, no pindirs
        sm_config_set_out_shift(&cfg, true, false, 8);  // shift right, no autopull
        sm_config_set_in_shift(&cfg, true, false, 8);   // shift right, no autopush
        pio_sm_set_pindirs_with_mask(gb_pio1, sm_idx,
            ((1 << 8) - 1) << GB_D0_PIN | (1 << GB_DATA_OE_PIN),
                    ((1 << 8) - 1) << GB_D0_PIN | (1 << GB_DATA_OE_PIN));
        pio_sm_set_pins_with_mask(gb_pio1, sm_idx,
            (1 << GB_DATA_OE_PIN),
            (1 << GB_DATA_OE_PIN));
        pio_sm_init(gb_pio1, sm_idx, output_prog_offset, &cfg);
        pio_sm_set_enabled(gb_pio1, sm_idx, true);
    }

    // ---- Set up DMA chain with sniffer ----
    setup_dma_chain();

    // --- Debug PIO: samples all GPIOs for bus capture ---
    mp_printf(&mp_plat_print, "setup debug program\n");
    build_debug_program();
    debug_pio = pio0;
    debug_sm = (int)pio_claim_unused_sm(debug_pio, false);
    if (debug_sm < 0) {
        mp_printf(&mp_plat_print, "gbio: no free PIO SM for debug\n");
    } else {
        // Load the debug program into PIO instruction memory.
        pio_program_t debug_prog_info = {
            .instructions = debug_program,
            .length = DEBUG_PROG_LEN,
            .origin = -1,
        };
        debug_prog_offset = pio_add_program(debug_pio, &debug_prog_info);
        mp_printf(&mp_plat_print, "gbio: debug program offset %d\n", debug_prog_offset);

        // Configure: in_base = GPIO0, autopush at 32 bits (all 32 low GPIOs).
        pio_sm_config cfg = pio_get_default_sm_config();
        sm_config_set_wrap(&cfg, debug_prog_offset, debug_prog_offset + DEBUG_PROG_LEN - 1);  // auto-wrap from last instruction back to 0
        sm_config_set_in_pins(&cfg, 0);
        sm_config_set_in_shift(&cfg, false, true, 32);  // shift right, autopush at 32 bits
        int err = pio_sm_init(debug_pio, debug_sm, debug_prog_offset, &cfg);
        if (err < 0) {
            mp_printf(&mp_plat_print, "gbio: debug sm init failed %d\n", err);
        }
        pio_sm_set_enabled(debug_pio, debug_sm, false);

        // Claim a DMA channel to drain the debug PIO's RX FIFO into the buffer.
        debug_dma_chan = (int)dma_claim_unused_channel(false);
        if (debug_dma_chan < 0) {
            mp_printf(&mp_plat_print, "gbio: no free DMA channel for debug\n");
            pio_remove_program(debug_pio, &debug_prog_info, debug_prog_offset);
            pio_sm_unclaim(debug_pio, debug_sm);
            debug_sm = -1;
        } else {
            mp_printf(&mp_plat_print, "gbio: debug dma channel %d\n", debug_dma_chan);
            debug_configured = true;
        }
    }

    // Initialize the 64K buffer: zero it, set up interrupt vectors and handlers
    memset(gb_data_buffer, 0x00, sizeof(gb_data_buffer));
    // Interrupt vectors: JP to fixed handlers
    gb_data_buffer[0x0040] = 0xC3;  // JP
    gb_data_buffer[0x0041] = (uint8_t)(VB_HANDLER_ADDR & 0xFF);
    gb_data_buffer[0x0042] = (uint8_t)(VB_HANDLER_ADDR >> 8);
    gb_data_buffer[0x0060] = 0xC3;  // JP
    gb_data_buffer[0x0061] = (uint8_t)(JP_HANDLER_ADDR & 0xFF);
    gb_data_buffer[0x0062] = (uint8_t)(JP_HANDLER_ADDR >> 8);
    // VBlank handler at VB_HANDLER_ADDR
    memcpy(gb_data_buffer + VB_HANDLER_ADDR, vblank_handler_prologue, sizeof(vblank_handler_prologue));
    // Joypad handler at JP_HANDLER_ADDR
    memcpy(gb_data_buffer + JP_HANDLER_ADDR, joypad_handler, sizeof(joypad_handler));
    // Idle spin at GB_IDLE_ADDR
    gb_data_buffer[GB_IDLE_ADDR] = 0xE9;  // JP (HL)

    // Keep our pins across VM resets; reset_port() resets GPIO/PIO generally.
    never_reset_pin_number(GB_RD_PIN);
    never_reset_pin_number(GB_WR_PIN);
    never_reset_pin_number(GB_CS_PIN);
    never_reset_pin_number(GB_CLK_PIN);
    for (uint8_t p = GB_A0_PIN; p <= GB_A15_PIN; p++) {
        never_reset_pin_number(p);
    }
    never_reset_pin_number(GB_DATA_OE_PIN);
    never_reset_pin_number(GB_RESET_PIN);
    for (uint8_t p = GB_D0_PIN; p <= GB_D7_PIN; p++) {
        never_reset_pin_number(p);
    }

    gbio_inited = true;
    mp_printf(&mp_plat_print, "gbio: init done\n");
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

    // Fill the 64K buffer with the DMG boot image at the correct
    // Game Boy addresses.  The boot ROM reads the logo from 0x0104
    // and the cartridge header from 0x0134.
    mp_printf(&mp_plat_print, "  [gbio] stage 2: filling 64K buffer with DMG boot image\n");
    memset(gb_data_buffer, 0x00, sizeof(gb_data_buffer));

    // ---- Interrupt vectors ----
    // 0x0040: VBlank – JP to VB_HANDLER_ADDR
    gb_data_buffer[0x0040] = 0xC3;
    gb_data_buffer[0x0041] = (uint8_t)(VB_HANDLER_ADDR & 0xFF);
    gb_data_buffer[0x0042] = (uint8_t)(VB_HANDLER_ADDR >> 8);
    // 0x0060: Joypad – JP to JP_HANDLER_ADDR
    gb_data_buffer[0x0060] = 0xC3;
    gb_data_buffer[0x0061] = (uint8_t)(JP_HANDLER_ADDR & 0xFF);
    gb_data_buffer[0x0062] = (uint8_t)(JP_HANDLER_ADDR >> 8);

    // ---- Entry point at 0x0100 ----
    // NOP; JP 0x0150  (the boot ROM jumps here after verification)
    gb_data_buffer[0x0100] = 0x00;                         // NOP
    gb_data_buffer[0x0101] = 0xC3;                         // JP
    gb_data_buffer[0x0102] = 0x50;                         // low byte of 0x0150
    gb_data_buffer[0x0103] = 0x01;                         // high byte

    // ---- Phase 1: Adafruit logo at 0x0104 (for the scroll animation) ----
    memcpy(gb_data_buffer + 0x0104, adafruit_logo, sizeof(adafruit_logo));

    // ---- Cartridge header at 0x0134 ----
    memcpy(gb_data_buffer + 0x0134, cartridge_header, sizeof(cartridge_header));

    // ---- Boot code at 0x0150 ----
    memcpy(gb_data_buffer + 0x0150, boot_code, sizeof(boot_code));

    // ---- Fixed interrupt handlers ----
    // VBlank handler at VB_HANDLER_ADDR
    memcpy(gb_data_buffer + VB_HANDLER_ADDR, vblank_handler_prologue, sizeof(vblank_handler_prologue));
    // Joypad handler at JP_HANDLER_ADDR
    memcpy(gb_data_buffer + JP_HANDLER_ADDR, joypad_handler, sizeof(joypad_handler));

    // ---- Idle spin at GB_IDLE_ADDR ----
    gb_data_buffer[GB_IDLE_ADDR] = 0xE9;  // JP (HL)

    // Start the DMA chain (address -> sniffer -> data -> output SM)
    mp_printf(&mp_plat_print, "  [gbio] stage 3: starting DMA chain\n");
    dma_channel_start(dma_addr_chan);
    dma_channel_start(dma_data_write_chan);

    // Release reset: the game boy boot ROM starts reading the cartridge header.
    // Start the debug PIO capture BEFORE releasing reset so we see the
    // very first bus transitions after the Game Boy wakes up.
    if (debug_configured) {
        mp_printf(&mp_plat_print, "  [gbio] stage 4a: starting debug PIO capture\n");
        // Fill the buffer with a canary so we can see which samples the DMA
        // actually overwrites vs. untouched.
        memset(debug_samples, 0xDE, sizeof(debug_samples));
        pio_sm_restart(debug_pio, debug_sm);
        pio_sm_clear_fifos(debug_pio, debug_sm);
        dma_channel_config dc = dma_channel_get_default_config(debug_dma_chan);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
        channel_config_set_dreq(&dc, pio_get_dreq(debug_pio, debug_sm, false));
        channel_config_set_read_increment(&dc, false);
        channel_config_set_write_increment(&dc, true);
        dma_channel_configure(debug_dma_chan, &dc,
            debug_samples,
            (void *)&debug_pio->rxf[debug_sm],
            DEBUG_SAMPLE_COUNT,
            true);
        pio_sm_set_enabled(debug_pio, debug_sm, true);
        if (dma_channel_is_busy(debug_dma_chan)) {
            mp_printf(&mp_plat_print, "  [gbio] debug dma channel busy\n");
        }
    }


    mp_printf(&mp_plat_print, "  [gbio] main tc %d\n", (unsigned)dma_channel_hw_addr(dma_addr_chan)->transfer_count);
    mp_printf(&mp_plat_print, "  [gbio] debug tc %d\n", (unsigned)dma_channel_hw_addr(debug_dma_chan)->transfer_count);

    mp_printf(&mp_plat_print, "  [gbio] stage 4: releasing /GB_RESET\n");
    gpio_put(GB_RESET_PIN, 0);

    // ---- Phase 2: swap to Nintendo logo after the display read ----
    // The boot ROM reads the logo twice: once for the scrolling animation
    // (early, ~200 ms after reset) and once for verification (late, ~1.5 s).
    // We start with the Adafruit logo so it appears during the scroll, then
    // overwrite it with the real Nintendo logo before verification.
    mp_printf(&mp_plat_print, "  [gbio] stage 4b: waiting 500 ms for logo display read\n");
    {
        uint32_t logo_start = supervisor_ticks_ms64();
        while (supervisor_ticks_ms64() - logo_start < 500) {
            RUN_BACKGROUND_TASKS;
        }
    }
    mp_printf(&mp_plat_print, "  [gbio] stage 4c: swapping Adafruit logo -> Nintendo logo at 0x0104\n");
    memcpy(gb_data_buffer + 0x0104, nintendo_logo, sizeof(nintendo_logo));

    // Wait for the boot sequence to complete.
    // The Game Boy reads ~256 bytes during boot at ~1 MHz = ~256 us.
    bool first_init = true;
    bool debug_printed = false;
    uint32_t last_tc = DEBUG_SAMPLE_COUNT;
    mp_printf(&mp_plat_print, "  [gbio] stage 5: waiting for boot to complete\n");
    uint32_t dma_spins = 0;
    uint32_t last_captured = 0;

    // Wait ~10 ms for the boot sequence, dumping debug samples
    uint32_t start_ms = supervisor_ticks_ms64();
    while (supervisor_ticks_ms64() - start_ms < 10) {
        RUN_BACKGROUND_TASKS;
        uint32_t tc = (uint32_t)dma_channel_hw_addr(dma_addr_chan)->transfer_count;
        if (debug_configured) {
            if (tc != last_tc) {
                mp_printf(&mp_plat_print, "  [gbio] debug tc %d\n", (unsigned)dma_channel_hw_addr(debug_dma_chan)->transfer_count);
            }
            uint32_t captured = DEBUG_SAMPLE_COUNT - dma_channel_hw_addr(debug_dma_chan)->transfer_count;
            for (uint32_t i = last_captured; i < captured; i++) {
                uint32_t s = debug_samples[i];
                if (s == 0xDEDEDEDE) {
                    continue;                   // skip canary (untouched buffer)
                }
                uint16_t addr = (uint16_t)((s >> 2) & 0xffff);  // A0..A15 at GPIO2..GPIO17
                uint8_t data = (uint8_t)((s >> 23) & 0xff);    // D0..D7 at GPIO23..GPIO30
                uint8_t rd = (uint8_t)((s >> GB_RD_PIN) & 1);         // /RD at GPIO20
                uint8_t clk = (uint8_t)((s >> GB_CLK_PIN) & 1);
                mp_printf(&mp_plat_print, "[%5lu] A=0x%04X D=0x%02X /RD=%u CLK=%u raw=0x%08X",
                    (unsigned long)i, addr, data, rd, clk, (unsigned)s);
                uint32_t main_tc = (uint32_t)dma_channel_hw_addr(dma_addr_chan)->transfer_count;
                mp_printf(&mp_plat_print, " main tc %d\n", main_tc);
            }
            last_captured = captured;
        }
        if (++dma_spins > 1000000) {
            if (tc != last_tc) {
                last_tc = tc;
                mp_printf(&mp_plat_print, "  [gbio] stage 5: still waiting (transfer_count=%u, gbc=%d)\n",
                    (unsigned)tc, (int)gameboy_color_booting);
            }
            dma_spins = 0;
        }
        last_tc = tc;
    }
    pio_sm_set_enabled(debug_pio, debug_sm, false);
    dma_channel_abort(debug_dma_chan);
    mp_printf(&mp_plat_print, "  [gbio] stage 5: boot wait complete (first_init=%d)\n", (int)first_init);


    uint32_t captured = DEBUG_SAMPLE_COUNT - dma_channel_hw_addr(debug_dma_chan)->transfer_count;
    for (uint32_t i = last_captured; i < captured; i++) {
        uint32_t s = debug_samples[i];
        if (s == 0xDEDEDEDE) {
            continue;                           // skip canary (untouched buffer)
        }
        uint16_t addr = (uint16_t)((s >> 2) & 0xffff);          // A0..A15 at GPIO2..GPIO17
        uint8_t data = (uint8_t)((s >> 23) & 0xff);            // D0..D7 at GPIO23..GPIO30
        uint8_t rd = (uint8_t)((s >> GB_RD_PIN) & 1);                 // /RD at GPIO20
        uint8_t clk = (uint8_t)((s >> GB_CLK_PIN) & 1);
        mp_printf(&mp_plat_print, "[%5lu] A=0x%04X D=0x%02X /RD=%u CLK=%u raw=0x%08X",
            (unsigned long)i, addr, data, rd, clk, (unsigned)s);
        uint32_t main_tc = (uint32_t)dma_channel_hw_addr(dma_addr_chan)->transfer_count;
        mp_printf(&mp_plat_print, " main tc %d\n", main_tc);
    }

    // Wait for the boot stream to be fully consumed.
    mp_printf(&mp_plat_print, "  [gbio] stage 6: feeder_wait_drained on active stream\n");
    feeder_wait_drained();

    if (!first_init) {
        gameboy_color = true;
    } else {
        gameboy_color = false;
    }
    mp_printf(&mp_plat_print, "  [gbio] stage 7: boot complete (gameboy_color=%d)\n", (int)gameboy_color);

    // If the debug capture wasn't triggered during stage 5, stop it now.
    if (debug_configured && !debug_printed) {
        pio_sm_set_enabled(debug_pio, debug_sm, false);
        dma_channel_abort(debug_dma_chan);
    }

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
    if (!gbio_inited || dma_data_read_chan < 0) {
        return;
    }
    if (!everything_going || supervisor_ticks_ms64() - last_vsync_time > 600) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("game boy not running"));
    }
    // Write commands into the vblank handler's user command area.
    // The handler executes them during the next vblank.
    if (len > VB_USER_AREA_SIZE) {
        len = VB_USER_AREA_SIZE;
    }
    memcpy(gb_data_buffer + VB_USER_AREA, buf, len);
    total_additional_cycles += additional_cycles;
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
    // Poll the frame counter in cartridge RAM (written by vblank handler)
    uint8_t start = gb_data_buffer[GB_RAM_VSYNC];
    while (gb_data_buffer[GB_RAM_VSYNC] == start) {
        RUN_BACKGROUND_TASKS;
    }
    vsync_count++;
    last_vsync_time = supervisor_ticks_ms64();
}

uint32_t common_hal_gbio_get_vsync_count(void) {
    return vsync_count;
}

uint8_t common_hal_gbio_get_pressed(void) {
    if (!gbio_inited) {
        return 0xff;
    }
    // Read button state from cartridge RAM (written by joypad handler).
    // The joypad handler writes the combined button/direction state to GB_RAM_GAMEPAD.
    // Bits: high nibble = buttons (Start, Select, B, A), low nibble = directions (D, U, L, R)
    // 0 = pressed, 1 = released (inverted from Game Boy convention)
    uint8_t raw = gb_data_buffer[GB_RAM_GAMEPAD];
    // Reset for next read
    gb_data_buffer[GB_RAM_GAMEPAD] = 0xFF;
    return raw;
}

bool common_hal_gbio_is_color(void) {
    return gameboy_color;
}
