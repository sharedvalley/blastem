#include <stdlib.h>
#include <string.h>
#include "rom_coverage.h"

uint8_t rom_coverage_active;

static const uint8_t *rom;
static uint32_t rom_size;
static rom_coverage_toggle toggle;
static void *toggle_data;
static uint8_t *bitmaps[COVERAGE_KINDS];

static size_t bitmap_size(void)
{
	return ((size_t)rom_size + 7) / 8;
}

void rom_coverage_init(const uint8_t *new_rom, uint32_t size, rom_coverage_toggle new_toggle, void *new_toggle_data)
{
	rom_coverage_active = 0;
	rom = new_rom;
	rom_size = size;
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
	if (!rom_coverage_active && toggle) {
		toggle(toggle_data, 1);
	}
	rom_coverage_active = 1;
}

void rom_coverage_stop(void)
{
	if (rom_coverage_active && toggle) {
		toggle(toggle_data, 0);
	}
	rom_coverage_active = 0;
}

void rom_coverage_mark(uint32_t offset, uint32_t size, uint8_t kind)
{
	uint8_t *bitmap = bitmaps[kind];
	for (uint32_t end = offset + size; offset < end && offset < rom_size; offset++)
	{
		bitmap[offset >> 3] |= 1 << (offset & 7);
	}
}

void rom_coverage_mark_ptr(const void *ptr, uint32_t size, uint8_t byte_in_word, uint8_t kind)
{
	//the pointer can be anywhere in the address space, so compare before subtracting
	if (!rom || (const uint8_t *)ptr < rom || (const uint8_t *)ptr >= rom + rom_size) {
		return;
	}
	uint32_t offset = (const uint8_t *)ptr - rom + byte_in_word;
	rom_coverage_mark(offset, size, COVERAGE_READ);
	if (kind != COVERAGE_READ) {
		rom_coverage_mark(offset, size, kind);
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
