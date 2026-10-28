#include <string.h>
#include "input.h"
#include "io.h"

typedef struct {
	input_tap_fun fun;
	void          *data;
} input_tap;

static pad_state live, applied;
static system_header *applied_system;
static input_source *source;
static input_tap taps[INPUT_MAX_TAPS];
static uint32_t num_taps;

static uint16_t *state_bits(pad_state *state, uint8_t pad)
{
	if (pad == GAMEPAD_MAIN_UNIT) {
		return &state->main_unit;
	}
	if (pad < 1 || pad > INPUT_MAX_PADS) {
		return NULL;
	}
	return state->pads + pad - 1;
}

void input_live_set(uint8_t pad, uint8_t button, uint8_t down)
{
	uint16_t *bits = state_bits(&live, pad);
	if (!bits || button >= 16) {
		return;
	}
	if (down) {
		*bits |= 1 << button;
	} else {
		*bits &= ~(1 << button);
	}
}

static void poll_live(pad_state *out)
{
	*out = live;
	//console buttons are presses, not something held, so each one lasts a frame
	live.main_unit = 0;
}

void input_set_source(input_source *src)
{
	input_source *old = source;
	source = src;
	if (old && old != src && old->close) {
		old->close(old);
	}
}

void input_add_tap(input_tap_fun fun, void *data)
{
	if (num_taps < INPUT_MAX_TAPS) {
		taps[num_taps++] = (input_tap){fun, data};
	}
}

void input_remove_tap(input_tap_fun fun, void *data)
{
	for (uint32_t i = 0; i < num_taps; i++)
	{
		if (taps[i].fun == fun && taps[i].data == data) {
			taps[i] = taps[--num_taps];
			return;
		}
	}
}

const pad_state *input_applied(void)
{
	return &applied;
}

static void apply_bits(system_header *system, uint8_t pad, uint16_t old, uint16_t new)
{
	uint16_t changed = old ^ new;
	for (uint8_t button = 0; changed; button++, changed >>= 1)
	{
		if (!(changed & 1)) {
			continue;
		}
		if (new & (1 << button)) {
			if (system->gamepad_down) {
				system->gamepad_down(system, pad, button);
			}
		} else if (system->gamepad_up) {
			system->gamepad_up(system, pad, button);
		}
	}
}

void input_reset_applied(system_header *system, const pad_state *state)
{
	//save states don't keep which buttons are down, so the system can still have the ones
	//held before the state was loaded: release every button state doesn't have down
	uint16_t buttons = ((1 << NUM_GAMEPAD_BUTTONS) - 1) & ~(1 << BUTTON_INVALID);
	for (uint8_t i = 0; i < INPUT_MAX_PADS; i++)
	{
		apply_bits(system, i + 1, ~state->pads[i] & buttons, state->pads[i]);
	}
	applied = *state;
	applied_system = system;
}

void input_frame(system_header *system)
{
	if (system != applied_system) {
		//a new system starts with nothing pressed
		memset(&applied, 0, sizeof(applied));
		applied_system = system;
	}
	pad_state next;
	if (!source || !source->poll(source, &next)) {
		input_set_source(NULL);
		poll_live(&next);
	} else {
		//console presses made while another source is active are dropped, not delayed
		live.main_unit = 0;
	}
	for (uint8_t i = 0; i < INPUT_MAX_PADS; i++)
	{
		apply_bits(system, i + 1, applied.pads[i], next.pads[i]);
	}
	apply_bits(system, GAMEPAD_MAIN_UNIT, applied.main_unit, next.main_unit);
	applied = next;
	//a tap may remove itself, so walk a copy
	input_tap current[INPUT_MAX_TAPS];
	uint32_t count = num_taps;
	memcpy(current, taps, count * sizeof(input_tap));
	for (uint32_t i = 0; i < count; i++)
	{
		current[i].fun(current[i].data, system, &applied);
	}
}
