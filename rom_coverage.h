#ifndef ROM_COVERAGE_H_
#define ROM_COVERAGE_H_

#include <stdint.h>
#include <stddef.h>

//Which bytes of the cartridge ROM the emulated system reads, as bitmaps indexed by
//offset in the ROM file. A playable quote keeps only the bytes some recording read.

enum {
	COVERAGE_READ,  //any read: CPU data and instruction fetches, DMA, the Z80 bank window
	COVERAGE_FETCH, //68K instruction words, opcodes and extension words
	COVERAGE_KINDS
};

extern uint8_t rom_coverage_active;

//called when marking starts and stops, for the system to route its CPU reads here
typedef void (*rom_coverage_toggle)(void *data, uint8_t enable);

//rom is the buffer the system reads the cartridge from, as it is in memory (Genesis
//byteswaps it in place), size the size of the ROM file
void rom_coverage_init(const uint8_t *rom, uint32_t size, rom_coverage_toggle toggle, void *toggle_data);
//clears the bitmaps and starts marking
void rom_coverage_start(void);
void rom_coverage_stop(void);
//marks size bytes starting at offset in the ROM file
void rom_coverage_mark(uint32_t offset, uint32_t size, uint8_t kind);
//marks size bytes read through a host pointer, if it points into the ROM buffer. The
//buffer must hold the ROM in file order or swapped within 16-bit words, and ptr must
//be the start of a word, so byte_in_word picks the byte of a 1-byte read.
void rom_coverage_mark_ptr(const void *ptr, uint32_t size, uint8_t byte_in_word, uint8_t kind);
const uint8_t *rom_coverage_bitmap(uint8_t kind);
size_t rom_coverage_bitmap_size(void);
uint32_t rom_coverage_rom_size(void);

#endif //ROM_COVERAGE_H_
