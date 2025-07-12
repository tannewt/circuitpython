/*
    GNU linker script for STM32L433 with filesystem
*/

/* Specify the memory areas */
MEMORY
{
    FLASH (rx)          : ORIGIN = 0x08000000, LENGTH = 256K /* entire flash */
    FLASH_ISR (rx)      : ORIGIN = 0x08000000, LENGTH = 16K /* ISR vector. */
    FLASH_FS (rw)       : ORIGIN = 0x08004000, LENGTH = 48K
    FLASH_FIRMWARE (rx) : ORIGIN = 0x08010000, LENGTH = 256K - 48K - 16K
    RAM (xrw)           : ORIGIN = 0x20000000, LENGTH = 64K
}

/* produce a link error if there is not this amount of RAM for these sections */
_minimum_stack_size = 24K;
_minimum_heap_size = 16K;

/* Define the top end of the stack.  The stack is full descending so begins just
   above last byte of RAM.  Note that EABI requires the stack to be 8-byte
   aligned for a call. */
_estack = ORIGIN(RAM) + LENGTH(RAM);

/* RAM extents for the garbage collector */
_ram_start = ORIGIN(RAM);
_ram_end = ORIGIN(RAM) + LENGTH(RAM);

/* ensure the firmware is within bounds */
ASSERT ( (ORIGIN(FLASH_FIRMWARE) + LENGTH(FLASH_FIRMWARE)) <= (ORIGIN(FLASH)+LENGTH(FLASH)), "FLASH_FIRMWARE out of bounds" );
