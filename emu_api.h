#ifndef EMU_API_H_
#define EMU_API_H_

#include <stdint.h>

//What an emulator core offers code that isn't tied to it, e.g. remixes: its memories by
//name. A core fills this in and nothing above this header needs to know how the core
//stores them, so another core or emulator gets the same features by doing the same.

typedef struct {
	const char *name;
	uint32_t   base;       //the address of its first byte, as the game sees it
	uint32_t   size;       //in bytes
	uint8_t    big_endian; //how the game puts bytes together into larger values
	void       *data;      //passed to read8 and write8
	//offset is from base, and is less than size
	uint8_t    (*read8)(void *data, uint32_t offset);
	void       (*write8)(void *data, uint32_t offset, uint8_t value);
} emu_region;

typedef struct {
	const char       *system; //e.g. "genesis", what a remix names to say what it's for
	const emu_region *regions;
	uint32_t         num_regions;
} emu_api;

#endif //EMU_API_H_
