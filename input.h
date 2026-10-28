#ifndef INPUT_H_
#define INPUT_H_

#include <stdint.h>
#include "system.h"

//Gamepad input as a state per emulated frame. The state comes from one active
//source (live input from the bindings by default, or a replay) and is applied to
//the system once per frame, at the end of the frame, so the same states always
//give the same run. Taps see every applied state, e.g. to record it.

#define INPUT_MAX_PADS 8
#define INPUT_MAX_TAPS 4

typedef struct {
	uint16_t pads[INPUT_MAX_PADS]; //pad n+1, bit b set while button b (DPAD_UP..BUTTON_MODE) is down
	uint16_t main_unit;            //buttons on the console itself, e.g. MAIN_UNIT_PAUSE
} pad_state;

typedef struct input_source input_source;
struct input_source {
	//fills out with the state of the next frame, returns 0 once the source has ended
	uint8_t (*poll)(input_source *src, pad_state *out);
	//called when the source ends or is replaced
	void    (*close)(input_source *src);
};

typedef void (*input_tap_fun)(void *data, system_header *system, const pad_state *applied);

//the bindings report every live gamepad change here
void input_live_set(uint8_t pad, uint8_t button, uint8_t down);
//NULL goes back to live input
void input_set_source(input_source *src);
void input_add_tap(input_tap_fun fun, void *data);
void input_remove_tap(input_tap_fun fun, void *data);
//the state the system currently has
const pad_state *input_applied(void);
//puts the system's buttons in state after its state changed under the input code, e.g.
//when a save state is loaded
void input_reset_applied(system_header *system, const pad_state *state);
//called once per emulated frame, at its end
void input_frame(system_header *system);

#endif //INPUT_H_
