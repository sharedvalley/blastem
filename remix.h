#ifndef REMIX_H_
#define REMIX_H_

#include <stdint.h>
#include <stddef.h>
#include "emu_api.h"

//A remix changes a run of a game and can give it an objective. It only knows a core
//through emu_api.h. It runs once per frame while a run is going, from the frame a run's
//state was loaded, and has no state of its own to save: a run starts it over.
//
//It is JSON, unknown keys are ignored:
//{
//  "format": 1, "system": "genesis",
//  "patches": [{"region": "wram", "address": "0xFFFE20", "size": 2, "value": 50,
//               "mode": "once" | "hold", "frame": 0, "until": 1800}],
//  "objective": {"timeLimit": 1800, "win": [CONDITION...], "lose": [CONDITION...]}
//}
//CONDITION is {"region", "address", "size", "op": "==" | "!=" | "<" | "<=" | ">" | ">=", "value"}
//A "once" patch writes on its frame. A "hold" patch writes each frame from its frame until
//"until", if it has one. All the conditions of a list have to hold for it to count.
//Each frame the patches apply, then the win conditions, the lose ones and the time limit
//are checked, and the first to hold ends the objective. size is 1, 2 or 4, 1 if left out.
//Numbers can be strings, for hexadecimal.

#define REMIX_MAX_SIZE (64 * 1024)

enum {
	REMIX_NONE,
	REMIX_WIN,
	REMIX_LOSE
};

//makes the size bytes of json the remix for api's core, returns 0 and keeps the one
//before if it can't be, which remix_why explains. Any run in progress stops being remixed.
uint8_t remix_parse(const char *json, size_t size, const emu_api *api);
void remix_discard(void);
const char *remix_why(void);
//starts a run of the current remix, if there's one and it's for api's core
void remix_start(const emu_api *api);
void remix_stop(void);
//called once per frame
void remix_frame(void);
//REMIX_NONE until the objective of the current run ended
uint8_t remix_outcome(void);
//the frame the objective ended on, or the frames run so far until it does
uint32_t remix_outcome_frame(void);
typedef void (*remix_outcome_handler)(uint8_t outcome, uint32_t frame);
void remix_on_outcome(remix_outcome_handler handler);
//reads the size byte value at address of the named region, returns 0 if there's no such
//place to read
uint8_t remix_peek(const emu_api *api, const char *region, uint32_t address, uint8_t size, uint32_t *out);

#endif //REMIX_H_
