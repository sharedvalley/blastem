#include <stdlib.h>
#include <string.h>
#include "rom_coverage.h"

uint8_t rom_coverage_active;

static const uint8_t *rom;
static uint32_t rom_size;
static uint8_t byteswapped;
static rom_coverage_toggle toggle;
static void *toggle_data;
static uint8_t *bitmaps[COVERAGE_KINDS];
static uint8_t marking;
static const uint8_t *valid;
static rom_coverage_miss miss;
static uint32_t data_misses;

static size_t bitmap_size(void)
{
	return ((size_t)rom_size + 7) / 8;
}

static uint8_t bit(const uint8_t *bitmap, uint32_t offset)
{
	return bitmap[offset >> 3] >> (offset & 7) & 1;
}

//routes the system's reads here while anything marks or checks them
static void update_active(void)
{
	uint8_t active = marking || valid;
	if (active != rom_coverage_active && toggle) {
		toggle(toggle_data, active);
	}
	rom_coverage_active = active;
}

void rom_coverage_init(const uint8_t *new_rom, uint32_t size, uint8_t new_byteswapped, rom_coverage_toggle new_toggle, void *new_toggle_data)
{
	//the previous system's reads are no longer routed anywhere
	rom_coverage_active = marking = 0;
	valid = NULL;
	rom = new_rom;
	rom_size = size;
	byteswapped = new_byteswapped;
	toggle = new_toggle;
	toggle_data = new_toggle_data;
	for (int i = 0; i < COVERAGE_KINDS; i++)
	{
		free(bitmaps[i]);
		bitmaps[i] = calloc(bitmap_size() ? bitmap_size() : 1, 1);
	}
}

void rom_coverage_start(void)
{
	if (!rom) {
		return;
	}
	for (int i = 0; i < COVERAGE_KINDS; i++)
	{
		memset(bitmaps[i], 0, bitmap_size());
	}
	marking = 1;
	update_active();
}

void rom_coverage_stop(void)
{
	marking = 0;
	update_active();
}

void rom_coverage_check(const uint8_t *new_valid, rom_coverage_miss new_miss)
{
	valid = rom ? new_valid : NULL;
	miss = new_miss;
	data_misses = 0;
	update_active();
}

uint32_t rom_coverage_data_misses(void)
{
	return data_misses;
}

void rom_coverage_mark_ptr(const void *ptr, uint32_t size, uint8_t byte_in_word, uint8_t kind, uint32_t address)
{
	//the pointer can be anywhere in the address space, so compare before subtracting
	if (!rom || (const uint8_t *)ptr < rom || (const uint8_t *)ptr >= rom + rom_size) {
		return;
	}
	uint32_t start = (const uint8_t *)ptr - rom + byte_in_word;
	uint32_t end = start + size < rom_size ? start + size : rom_size;
	for (uint32_t offset = start; offset < end; offset++)
	{
		if (marking) {
			bitmaps[COVERAGE_READ][offset >> 3] |= 1 << (offset & 7);
			if (kind != COVERAGE_READ) {
				bitmaps[kind][offset >> 3] |= 1 << (offset & 7);
			}
		}
		if (valid && !bit(valid, offset)) {
			if (kind == COVERAGE_FETCH) {
				if (miss) {
					miss(offset, address);
				}
				return;
			}
			data_misses++;
		}
	}
}

const uint8_t *rom_coverage_bitmap(uint8_t kind)
{
	return bitmaps[kind];
}

size_t rom_coverage_bitmap_size(void)
{
	return rom ? bitmap_size() : 0;
}

uint32_t rom_coverage_rom_size(void)
{
	return rom ? rom_size : 0;
}

uint8_t rom_coverage_rom_byte(uint32_t offset)
{
	return offset < rom_size ? rom[byteswapped ? offset ^ 1 : offset] : 0;
}
