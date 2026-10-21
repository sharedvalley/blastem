#ifndef INPUT_RECORD_H_
#define INPUT_RECORD_H_

#include <stdint.h>
#include <stddef.h>
#include "system.h"

enum {
	INPUT_RECORD_OFF,
	INPUT_RECORD_RECORDING,
	INPUT_RECORD_PLAYING
};

//called in place of system->gamepad_down/gamepad_up for content bindings
void input_record_gamepad(uint8_t down, uint8_t pad, uint8_t button);
//called once per emulated frame, at a point that only depends on emulated state
void input_record_frame_end(void);
//called between runs of the system, the only place states are taken or loaded
void input_record_run_boundary(system_header *system);

void record_start(void);
void record_stop(void);
uint8_t *record_data(size_t *size_out);
uint8_t playback_start(uint8_t *data, size_t size);
void playback_stop(void);
uint32_t record_status(void);

#endif //INPUT_RECORD_H_
