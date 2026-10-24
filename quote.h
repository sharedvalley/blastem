#ifndef QUOTE_H_
#define QUOTE_H_

#include <stdint.h>
#include <stddef.h>

//Playable quotes: an official recording plus only the ROM bytes it and the other
//passes from the same start read, so it plays, and can be played, without the ROM.
//
//File (.pq): "PQUO" | version | chunks, each a 4 character tag | size | data, all
//little endian. Readers skip chunks they don't know, so new data goes in a new chunk.
//META "key=value" lines: rom_sha1 and rom_size of the original ROM, name, passes, kept
//ROMS ranges of the ROM file: offset | length | bytes, everything else reads as 0
//RPLY the official recording, as record_data() returns it

//a recording or playback that finished, as a pass of the quote being built. Passes of
//one quote start from the same state, so one from another state starts a new quote.
void quote_add_pass(const uint8_t *recording, size_t size, const uint8_t *start_state, size_t state_size, const uint8_t *read_bitmap);
uint32_t quote_passes(void);
//builds a quote of the passes so far and returns it in a buffer the caller frees, or
//NULL without passes or a ROM to cut. official is the recording it replays, which
//starts from official_state, the passes' start, or NULL for the first pass.
uint8_t *quote_build(const uint8_t *official, size_t official_size, const uint8_t *official_state, size_t official_state_size, const char *name, size_t *size);
//bytes of the ROM the passes so far read, header included, and the ROM's size
uint32_t quote_kept_bytes(uint32_t *rom_size);
//builds a quote of the passes so far, the first one its replay, and writes it to path,
//reporting how much of the ROM it kept
uint8_t quote_save(const char *name, const char *path);

//if buffer holds a quote, replaces it with the ROM the quote keeps, zeros elsewhere,
//and returns the ROM's size; returns 0 for anything else
uint32_t quote_unpack(void **buffer, uint32_t size);
//whether rom, the buffer a system was made with, is a loaded quote's
uint8_t quote_loaded(const void *rom);
//the official recording of the loaded quote
const uint8_t *quote_replay(size_t *size);
//the SHA-1 of the original ROM, in hex, when rom is the loaded quote's
const char *quote_rom_sha1(const uint8_t *rom);

//called the first time the system fetches an instruction the loaded quote left out
typedef void (*quote_miss_handler)(uint32_t rom_offset, uint32_t address);
//checks the system's reads against the loaded quote from now on
void quote_check(quote_miss_handler handler);
//the system is back at a state in the quote, so a later miss is reported again
void quote_rearm(void);

#endif //QUOTE_H_
