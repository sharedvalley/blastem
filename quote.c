#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quote.h"
#include "rom_coverage.h"
#include "hash.h"
#include "util.h"

#define QUOTE_MAGIC "PQUO"
//the layout, not the contents: readers skip chunks they don't know, so new data goes
//in a new chunk and only a change to an existing chunk needs a new version
#define QUOTE_VERSION 1
//the vectors and the header: the CPU reads the vectors on exceptions and rom.db
//identifies a cartridge by its header
#define HEADER_SIZE 0x200
//a cartridge locked on to Sonic & Knuckles is identified by the header of its own
//half, which a recording may never read
#define LOCK_ON_START 0x200000

typedef struct {
	uint8_t *recording;
	size_t  size;
} pass;

//the quote being built
static pass *passes;
static uint32_t num_passes, passes_storage;
static uint8_t *start_state;
static size_t start_state_size;
static uint8_t *union_read;
static uint32_t union_rom_size;

//the quote loaded in place of a ROM
static uint8_t *loaded_rom;
static uint8_t *loaded_valid;
static char loaded_sha1[41];
static uint8_t *loaded_replay;
static size_t loaded_replay_size;
static quote_miss_handler miss_handler;
static uint8_t missed;

static void set_bit(uint8_t *bitmap, uint32_t offset)
{
	bitmap[offset >> 3] |= 1 << (offset & 7);
}

static uint8_t get_bit(const uint8_t *bitmap, uint32_t offset)
{
	return bitmap[offset >> 3] >> (offset & 7) & 1;
}

static void reset_passes(void)
{
	for (uint32_t i = 0; i < num_passes; i++)
	{
		free(passes[i].recording);
	}
	num_passes = 0;
	free(start_state);
	start_state = NULL;
	free(union_read);
	union_read = NULL;
}

void quote_add_pass(const uint8_t *recording, size_t size, const uint8_t *state, size_t state_size, const uint8_t *read_bitmap)
{
	uint32_t rom_size = rom_coverage_rom_size();
	if (!rom_size || loaded_rom) {
		//a quote is only cut from a whole ROM
		return;
	}
	if (num_passes && (state_size != start_state_size || memcmp(state, start_state, state_size) || rom_size != union_rom_size)) {
		reset_passes();
	}
	if (!num_passes) {
		start_state = malloc(state_size);
		memcpy(start_state, state, state_size);
		start_state_size = state_size;
		union_rom_size = rom_size;
		union_read = calloc((rom_size + 7) / 8, 1);
	}
	if (num_passes == passes_storage) {
		passes_storage = passes_storage ? passes_storage * 2 : 4;
		passes = realloc(passes, passes_storage * sizeof(pass));
	}
	passes[num_passes].recording = malloc(size);
	memcpy(passes[num_passes].recording, recording, size);
	passes[num_passes++].size = size;
	for (size_t i = 0; i < (rom_size + 7) / 8; i++)
	{
		union_read[i] |= read_bitmap[i];
	}
}

uint32_t quote_passes(void)
{
	return num_passes;
}

//the passes' reads plus the headers rom.db needs
static uint8_t *kept_bitmap(void)
{
	uint8_t *kept = malloc((union_rom_size + 7) / 8);
	memcpy(kept, union_read, (union_rom_size + 7) / 8);
	for (uint32_t offset = 0; offset < HEADER_SIZE && offset < union_rom_size; offset++)
	{
		set_bit(kept, offset);
	}
	for (uint32_t offset = LOCK_ON_START; offset < LOCK_ON_START + HEADER_SIZE && offset < union_rom_size; offset++)
	{
		set_bit(kept, offset);
	}
	return kept;
}

uint32_t quote_kept_bytes(uint32_t *rom_size)
{
	*rom_size = num_passes ? union_rom_size : 0;
	if (!num_passes) {
		return 0;
	}
	uint8_t *kept = kept_bitmap();
	uint32_t count = 0;
	for (uint32_t offset = 0; offset < union_rom_size; offset++)
	{
		count += get_bit(kept, offset);
	}
	free(kept);
	return count;
}

typedef struct {
	uint8_t *data;
	size_t  size, storage;
} buffer;

static void append(buffer *buf, const void *data, size_t size)
{
	if (buf->size + size > buf->storage) {
		buf->storage = buf->storage * 2 > buf->size + size ? buf->storage * 2 : buf->size + size;
		buf->data = realloc(buf->data, buf->storage);
	}
	memcpy(buf->data + buf->size, data, size);
	buf->size += size;
}

static void append32(buffer *buf, uint32_t value)
{
	uint8_t bytes[4] = {value, value >> 8, value >> 16, value >> 24};
	append(buf, bytes, 4);
}

//starts a chunk and returns where its size goes, for end_chunk
static size_t start_chunk(buffer *buf, const char *tag)
{
	append(buf, tag, 4);
	append32(buf, 0);
	return buf->size - 4;
}

static void end_chunk(buffer *buf, size_t size_at)
{
	uint32_t size = buf->size - size_at - 4;
	uint8_t bytes[4] = {size, size >> 8, size >> 16, size >> 24};
	memcpy(buf->data + size_at, bytes, 4);
}

uint8_t *quote_build(const uint8_t *official, size_t official_size, const uint8_t *official_state, size_t official_state_size, const char *name, size_t *size)
{
	if (!num_passes || rom_coverage_rom_size() != union_rom_size) {
		return NULL;
	}
	if (!official) {
		official = passes[0].recording;
		official_size = passes[0].size;
	} else if (official_state_size != start_state_size || memcmp(official_state, start_state, start_state_size)) {
		//the passes' reads only cover what can happen from their start
		return NULL;
	}
	//the ROM in file order, for its hash and the kept bytes
	uint8_t *rom = malloc(union_rom_size);
	for (uint32_t offset = 0; offset < union_rom_size; offset++)
	{
		rom[offset] = rom_coverage_rom_byte(offset);
	}
	uint8_t raw_sha1[20], sha1_hex[41];
	sha1(rom, union_rom_size, raw_sha1);
	bin_to_hex(sha1_hex, raw_sha1, 20);
	uint8_t *kept = kept_bitmap();
	uint32_t kept_bytes = 0;
	for (uint32_t offset = 0; offset < union_rom_size; offset++)
	{
		kept_bytes += get_bit(kept, offset);
	}

	buffer out = {0};
	append(&out, QUOTE_MAGIC, 4);
	append32(&out, QUOTE_VERSION);

	char meta[512];
	snprintf(meta, sizeof(meta), "rom_sha1=%s\nrom_size=%u\nname=%s\npasses=%u\nkept=%u\n",
		(char *)sha1_hex, union_rom_size, name ? name : "", num_passes, kept_bytes);
	size_t chunk = start_chunk(&out, "META");
	append(&out, meta, strlen(meta));
	end_chunk(&out, chunk);

	chunk = start_chunk(&out, "ROMS");
	for (uint32_t offset = 0; offset < union_rom_size;)
	{
		if (!get_bit(kept, offset)) {
			offset++;
			continue;
		}
		uint32_t end = offset;
		while (end < union_rom_size && get_bit(kept, end))
		{
			end++;
		}
		append32(&out, offset);
		append32(&out, end - offset);
		append(&out, rom + offset, end - offset);
		offset = end;
	}
	end_chunk(&out, chunk);

	chunk = start_chunk(&out, "RPLY");
	append(&out, official, official_size);
	end_chunk(&out, chunk);

	free(kept);
	free(rom);
	*size = out.size;
	return out.data;
}

uint8_t quote_save(const char *name, const char *path)
{
	size_t size;
	uint8_t *data = quote_build(NULL, 0, NULL, 0, name, &size);
	if (!data) {
		warning("There are no passes to make a playable quote of: record or play a recording to the end first\n");
		return 0;
	}
	FILE *f = fopen(path, "wb");
	uint8_t written = f && fwrite(data, 1, size, f) == size;
	if (f && fclose(f)) {
		written = 0;
	}
	free(data);
	if (!written) {
		warning("Failed to write the playable quote to %s\n", path);
		return 0;
	}
	uint32_t rom_size, kept = quote_kept_bytes(&rom_size);
	printf("Saved a playable quote of %u pass%s to %s: %u bytes of the ROM (%.1f%%), %u bytes in all\n",
		num_passes, num_passes == 1 ? "" : "es", path, kept, 100.0 * kept / rom_size, (uint32_t)size);
	return 1;
}

static uint32_t read32(const uint8_t *src)
{
	return src[0] | src[1] << 8 | src[2] << 16 | (uint32_t)src[3] << 24;
}

//the value of key in META's lines, copied into value
static uint8_t meta_value(const uint8_t *meta, uint32_t size, const char *key, char *value, size_t value_size)
{
	size_t key_size = strlen(key);
	for (const uint8_t *line = meta, *end = meta + size; line < end;)
	{
		const uint8_t *line_end = memchr(line, '\n', end - line);
		if (!line_end) {
			line_end = end;
		}
		if (line_end - line > key_size && !memcmp(line, key, key_size) && line[key_size] == '=') {
			size_t length = line_end - line - key_size - 1;
			if (length >= value_size) {
				return 0;
			}
			memcpy(value, line + key_size + 1, length);
			value[length] = 0;
			return 1;
		}
		line = line_end + 1;
	}
	return 0;
}

uint32_t quote_unpack(void **vbuffer, uint32_t size)
{
	uint8_t *data = *vbuffer;
	if (size < 8 || memcmp(data, QUOTE_MAGIC, 4)) {
		return 0;
	}
	if (read32(data + 4) != QUOTE_VERSION) {
		warning("This playable quote is from a newer version of BlastEm\n");
		return 0;
	}
	const uint8_t *meta = NULL, *ranges = NULL, *replay = NULL;
	uint32_t meta_size = 0, ranges_size = 0, replay_size = 0;
	for (const uint8_t *cur = data + 8, *end = data + size; cur < end;)
	{
		if (end - cur < 8 || read32(cur + 4) > end - cur - 8) {
			warning("This playable quote is damaged\n");
			return 0;
		}
		uint32_t chunk_size = read32(cur + 4);
		if (!memcmp(cur, "META", 4)) {
			meta = cur + 8;
			meta_size = chunk_size;
		} else if (!memcmp(cur, "ROMS", 4)) {
			ranges = cur + 8;
			ranges_size = chunk_size;
		} else if (!memcmp(cur, "RPLY", 4)) {
			replay = cur + 8;
			replay_size = chunk_size;
		}
		cur += 8 + chunk_size;
	}
	char sha1_hex[41], rom_size_text[16];
	if (!meta || !ranges || !replay || !meta_value(meta, meta_size, "rom_sha1", sha1_hex, sizeof(sha1_hex))
		|| strlen(sha1_hex) != 40 || !meta_value(meta, meta_size, "rom_size", rom_size_text, sizeof(rom_size_text))) {
		warning("This playable quote is missing parts\n");
		return 0;
	}
	uint32_t rom_size = strtoul(rom_size_text, NULL, 10);
	if (!rom_size) {
		return 0;
	}
	//load_media's buffers are padded to a power of two, which byteswapping relies on
	uint8_t *rom = aligned_calloc(1, nearest_pow2(rom_size), 16);
	uint8_t *valid = calloc((rom_size + 7) / 8, 1);
	for (const uint8_t *cur = ranges, *end = ranges + ranges_size; cur < end;)
	{
		uint32_t offset, length;
		if (end - cur < 8 || (offset = read32(cur)) > rom_size || (length = read32(cur + 4)) > rom_size - offset
			|| length > end - cur - 8) {
			warning("This playable quote is damaged\n");
			aligned_free(rom);
			free(valid);
			return 0;
		}
		memcpy(rom + offset, cur + 8, length);
		for (uint32_t i = offset; i < offset + length; i++)
		{
			set_bit(valid, i);
		}
		cur += 8 + length;
	}
	free(loaded_valid);
	free(loaded_replay);
	loaded_rom = rom;
	loaded_valid = valid;
	memcpy(loaded_sha1, sha1_hex, sizeof(loaded_sha1));
	loaded_replay = malloc(replay_size);
	memcpy(loaded_replay, replay, replay_size);
	loaded_replay_size = replay_size;
	aligned_free(data);
	*vbuffer = rom;
	return rom_size;
}

uint8_t quote_loaded(const void *rom)
{
	return rom && rom == loaded_rom;
}

const uint8_t *quote_replay(size_t *size)
{
	*size = loaded_replay_size;
	return loaded_replay;
}

const char *quote_rom_sha1(const uint8_t *rom)
{
	return rom && rom == loaded_rom ? loaded_sha1 : NULL;
}

static void report_miss(uint32_t offset, uint32_t address)
{
	if (missed) {
		return;
	}
	missed = 1;
	if (miss_handler) {
		miss_handler(offset, address);
	}
}

void quote_check(quote_miss_handler handler)
{
	if (!loaded_rom) {
		return;
	}
	miss_handler = handler;
	missed = 0;
	rom_coverage_check(loaded_valid, report_miss);
}

void quote_rearm(void)
{
	missed = 0;
}
