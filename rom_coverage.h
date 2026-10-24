#ifndef ROM_COVERAGE_H_
#define ROM_COVERAGE_H_

#include <stdint.h>
#include <stddef.h>

//Which bytes of the cartridge ROM the emulated system reads, as bitmaps indexed by
//offset in the ROM file. A playable quote keeps only the bytes some recording read,
//and while one plays, reads are checked against the bytes it kept.

enum {
	COVERAGE_READ,  //any read: CPU data and instruction fetches, DMA, the Z80 bank window
	COVERAGE_FETCH, //68K instruction words, opcodes and extension words
	COVERAGE_KINDS
};

//set while reads are marked or checked, for the hooks in the read paths
extern uint8_t rom_coverage_active;

//called when marking or checking starts and stops, for the system to route its CPU
//reads here
typedef void (*rom_coverage_toggle)(void *data, uint8_t enable);
//called when an instruction is fetched from a byte the quote left out, with the ROM
//offset and the CPU address it was fetched from
typedef void (*rom_coverage_miss)(uint32_t offset, uint32_t address);

//rom is the buffer the system reads the cartridge from, size the size of the ROM file
//and byteswapped whether the buffer holds it with the bytes of each 16-bit word
//swapped, as Genesis does on little endian hosts
void rom_coverage_init(const uint8_t *rom, uint32_t size, uint8_t byteswapped, rom_coverage_toggle toggle, void *toggle_data);
//clears the bitmaps and starts marking
void rom_coverage_start(void);
void rom_coverage_stop(void);
//checks reads against valid, one bit per byte of the ROM file, until called with NULL
void rom_coverage_check(const uint8_t *valid, rom_coverage_miss miss);
//data reads of bytes left out since checking started; they read 0
uint32_t rom_coverage_data_misses(void);
//marks or checks size bytes read through a host pointer, if it points into the ROM
//buffer. ptr must be the start of a 16-bit word, so byte_in_word picks the byte of a
//1-byte read. address is the CPU address read, for reporting a miss.
void rom_coverage_mark_ptr(const void *ptr, uint32_t size, uint8_t byte_in_word, uint8_t kind, uint32_t address);
const uint8_t *rom_coverage_bitmap(uint8_t kind);
size_t rom_coverage_bitmap_size(void);
uint32_t rom_coverage_rom_size(void);
//the byte at offset in the ROM file
uint8_t rom_coverage_rom_byte(uint32_t offset);

#endif //ROM_COVERAGE_H_
