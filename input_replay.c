//Records gamepad input and plays it back. Recording is a tap on the input applied
//each frame (input.h), playback an input source, so neither needs to know where
//input comes from, and the live, recorded and replayed paths are the same code.
#include <stdlib.h>
#include <string.h>
#include "input_replay.h"
#include "input.h"
#include "io.h"
#include "zlib/zlib.h"
#include "util.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
//EMSCRIPTEN_KEEPALIVE exports a function and puts it on Module as _name, so JS can
//call it without the function being listed in the Makefile's EXPORTED_FUNCTIONS.
//Memory crossing to JS is owned here: JS only copies through Module.HEAPU8.
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

#define RECORD_MAGIC "BREC"
#define RECORD_VERSION 2
#define CHECKPOINT_FRAMES 60
//pads plus the main unit as 16-bit values, padded to a multiple of 4 bytes
#define PAD_STATE_BYTES 20

typedef struct {
	uint32_t frame;
	uint8_t  pad;
	uint8_t  button;
	uint8_t  down;
	uint8_t  unused;
} input_event;

//a finished or loaded recording: one buffer, read in place
typedef struct {
	uint32_t  frames;
	pad_state initial;
	uint8_t   *state;
	uint32_t  state_size;
	uint8_t   *events;
	uint32_t  num_events;
	uint8_t   *checkpoints;
	uint32_t  num_checkpoints;
} recording_view;

static uint8_t *recording;
static size_t recording_size;
static recording_view view;

//a recording in progress
static uint8_t recording_active;
static uint8_t *rec_state;
static size_t rec_state_size;
static pad_state rec_initial, rec_last;
static input_event *rec_events;
static uint32_t rec_num_events, rec_events_storage;
static uint32_t *rec_checkpoints;
static uint32_t rec_num_checkpoints, rec_checkpoints_storage;
static uint32_t rec_frames;

//playback
static uint8_t playing, stopping;
static pad_state play_state;
static uint32_t play_frame, play_next_event;
static uint8_t reported_desync;

//recordings coming from JS
static uint8_t *exchange;
static size_t exchange_size;

enum {
	PENDING_NONE,
	PENDING_RECORD,
	PENDING_PLAYBACK
};
static uint8_t pending;

#ifdef __EMSCRIPTEN__
EM_JS(void, notify_playback_end, (void), {
	if (Module.onBlastemPlaybackEnd) {
		Module.onBlastemPlaybackEnd();
	}
});
EM_JS(void, notify_desync, (uint32_t frame), {
	if (Module.onBlastemDesync) {
		Module.onBlastemDesync(frame);
	}
});
#else
static void notify_playback_end(void) {}
static void notify_desync(uint32_t frame) {}
#endif

static uint32_t state_crc(system_header *system)
{
	size_t size;
	uint8_t *state = system->serialize(system, &size);
	uint32_t crc = crc32(0, state, size);
	free(state);
	return crc;
}

static void *grow(void *array, uint32_t count, uint32_t *storage, size_t item_size)
{
	if (count < *storage) {
		return array;
	}
	*storage = *storage ? *storage * 2 : 256;
	return realloc(array, *storage * item_size);
}

static void record_tap(void *data, system_header *system, const pad_state *applied)
{
	for (uint8_t i = 0; i <= INPUT_MAX_PADS; i++)
	{
		uint16_t old = i < INPUT_MAX_PADS ? rec_last.pads[i] : rec_last.main_unit;
		uint16_t new = i < INPUT_MAX_PADS ? applied->pads[i] : applied->main_unit;
		for (uint8_t button = 0; old != new; button++)
		{
			uint16_t bit = 1 << button;
			if ((old ^ new) & bit) {
				rec_events = grow(rec_events, rec_num_events, &rec_events_storage, sizeof(input_event));
				rec_events[rec_num_events++] = (input_event){
					.frame = rec_frames,
					.pad = i < INPUT_MAX_PADS ? i + 1 : GAMEPAD_MAIN_UNIT,
					.button = button,
					.down = (new & bit) != 0
				};
				old ^= bit;
			}
		}
	}
	rec_last = *applied;
	rec_frames++;
	if (rec_frames % CHECKPOINT_FRAMES == 0 && system->serialize) {
		rec_checkpoints = grow(rec_checkpoints, rec_num_checkpoints, &rec_checkpoints_storage, sizeof(uint32_t));
		rec_checkpoints[rec_num_checkpoints++] = state_crc(system);
	}
}

static input_event read_event(uint32_t index)
{
	input_event event;
	memcpy(&event, view.events + index * sizeof(input_event), sizeof(event));
	return event;
}

static uint32_t read_checkpoint(uint32_t index)
{
	uint32_t crc;
	memcpy(&crc, view.checkpoints + index * sizeof(uint32_t), sizeof(crc));
	return crc;
}

static uint8_t replay_poll(input_source *src, pad_state *out)
{
	if (play_frame >= view.frames) {
		return 0;
	}
	for (; play_next_event < view.num_events; play_next_event++)
	{
		input_event event = read_event(play_next_event);
		if (event.frame != play_frame) {
			break;
		}
		uint16_t *bits = event.pad == GAMEPAD_MAIN_UNIT ? &play_state.main_unit
			: event.pad >= 1 && event.pad <= INPUT_MAX_PADS ? play_state.pads + event.pad - 1 : NULL;
		if (bits && event.button < 16) {
			if (event.down) {
				*bits |= 1 << event.button;
			} else {
				*bits &= ~(1 << event.button);
			}
		}
	}
	*out = play_state;
	play_frame++;
	return 1;
}

//compares the state with the recording's at the same points the recording took it
static void verify_tap(void *data, system_header *system, const pad_state *applied)
{
	if (play_frame % CHECKPOINT_FRAMES || reported_desync || !system->serialize) {
		return;
	}
	uint32_t index = play_frame / CHECKPOINT_FRAMES - 1;
	if (index < view.num_checkpoints && read_checkpoint(index) != state_crc(system)) {
		reported_desync = 1;
		warning("Playback desynced from its recording by frame %u\n", play_frame);
		notify_desync(play_frame);
	}
}

static void replay_close(input_source *src)
{
	playing = 0;
	input_remove_tap(verify_tap, NULL);
	if (!stopping) {
		notify_playback_end();
	}
}

static input_source replay_source = {
	.poll = replay_poll,
	.close = replay_close
};

static uint8_t *put16(uint8_t *dst, uint16_t value)
{
	memcpy(dst, &value, sizeof(value));
	return dst + sizeof(value);
}

static uint8_t *put32(uint8_t *dst, uint32_t value)
{
	memcpy(dst, &value, sizeof(value));
	return dst + sizeof(value);
}

static uint8_t get32(uint8_t **cur, uint8_t *end, uint32_t *value)
{
	if (end - *cur < 4) {
		return 0;
	}
	memcpy(value, *cur, sizeof(*value));
	*cur += 4;
	return 1;
}

//"BREC" | version | frames | pad state at the start | state size | state
//| event count | events | checkpoint count | checkpoints
//all values are little endian, as wasm is
static uint8_t parse_recording(uint8_t *data, size_t size, recording_view *out)
{
	uint8_t *cur = data, *end = data + size;
	uint32_t version;
	if (size < 4 || memcmp(cur, RECORD_MAGIC, 4)) {
		return 0;
	}
	cur += 4;
	if (!get32(&cur, end, &version) || version != RECORD_VERSION || !get32(&cur, end, &out->frames)
		|| end - cur < PAD_STATE_BYTES) {
		return 0;
	}
	for (uint32_t i = 0; i < INPUT_MAX_PADS; i++)
	{
		memcpy(out->initial.pads + i, cur + i * 2, 2);
	}
	memcpy(&out->initial.main_unit, cur + INPUT_MAX_PADS * 2, 2);
	cur += PAD_STATE_BYTES;
	if (!get32(&cur, end, &out->state_size) || end - cur < out->state_size) {
		return 0;
	}
	out->state = cur;
	cur += out->state_size;
	if (!get32(&cur, end, &out->num_events) || (end - cur) / sizeof(input_event) < out->num_events) {
		return 0;
	}
	out->events = cur;
	cur += out->num_events * sizeof(input_event);
	if (!get32(&cur, end, &out->num_checkpoints) || (end - cur) / sizeof(uint32_t) < out->num_checkpoints) {
		return 0;
	}
	out->checkpoints = cur;
	return 1;
}

//packs the recording in progress into the one buffer a finished recording lives in
static void finish_recording(void)
{
	size_t size = 4 + 4 * 2 + PAD_STATE_BYTES + 4 + rec_state_size + 4 + rec_num_events * sizeof(input_event)
		+ 4 + rec_num_checkpoints * sizeof(uint32_t);
	uint8_t *data = malloc(size), *cur = data;
	memcpy(cur, RECORD_MAGIC, 4);
	cur = put32(cur + 4, RECORD_VERSION);
	cur = put32(cur, rec_frames);
	uint8_t *pads = cur;
	for (uint32_t i = 0; i < INPUT_MAX_PADS; i++)
	{
		cur = put16(cur, rec_initial.pads[i]);
	}
	cur = put16(cur, rec_initial.main_unit);
	memset(cur, 0, PAD_STATE_BYTES - (cur - pads));
	cur = put32(pads + PAD_STATE_BYTES, rec_state_size);
	memcpy(cur, rec_state, rec_state_size);
	cur = put32(cur + rec_state_size, rec_num_events);
	memcpy(cur, rec_events, rec_num_events * sizeof(input_event));
	cur = put32(cur + rec_num_events * sizeof(input_event), rec_num_checkpoints);
	memcpy(cur, rec_checkpoints, rec_num_checkpoints * sizeof(uint32_t));
	free(recording);
	recording = data;
	recording_size = size;
	parse_recording(recording, recording_size, &view);
}

void input_replay_run_boundary(system_header *system)
{
	if (!pending || !system->serialize || !system->deserialize) {
		return;
	}
	if (pending == PENDING_RECORD) {
		free(rec_state);
		rec_state = system->serialize(system, &rec_state_size);
		rec_initial = rec_last = *input_applied();
		rec_num_events = rec_num_checkpoints = rec_frames = 0;
		input_remove_tap(record_tap, NULL);
		input_add_tap(record_tap, NULL);
		recording_active = 1;
	} else {
		system->deserialize(system, view.state, view.state_size);
		//the loaded state has the recording's pads down, not the ones applied before
		input_reset_applied(&view.initial);
		play_state = view.initial;
		play_frame = play_next_event = 0;
		reported_desync = 0;
		input_add_tap(verify_tap, NULL);
		input_set_source(&replay_source);
		playing = 1;
	}
	pending = PENDING_NONE;
}

EXPORT void record_start(void)
{
	playback_stop();
	pending = PENDING_RECORD;
}

EXPORT void record_stop(void)
{
	if (pending == PENDING_RECORD) {
		pending = PENDING_NONE;
	}
	if (recording_active) {
		recording_active = 0;
		input_remove_tap(record_tap, NULL);
		finish_recording();
	}
}

//the current recording, valid until the next recording or load
EXPORT uint8_t *record_data(void)
{
	return recording;
}

EXPORT size_t record_data_size(void)
{
	return recording ? recording_size : 0;
}

//returns a buffer for JS to copy a recording into before calling playback_start
EXPORT uint8_t *playback_buffer(size_t size)
{
	exchange = realloc(exchange, size ? size : 1);
	exchange_size = size;
	return exchange;
}

//plays the size bytes JS copied into playback_buffer, or the current recording when size is 0
EXPORT uint8_t playback_start(size_t size)
{
	record_stop();
	if (size) {
		recording_view loaded;
		if (size > exchange_size || !parse_recording(exchange, size, &loaded)) {
			return 0;
		}
		playback_stop();
		//the loaded buffer becomes the recording, nothing is copied
		free(recording);
		recording = exchange;
		recording_size = size;
		view = loaded;
		exchange = NULL;
		exchange_size = 0;
	} else if (!recording) {
		return 0;
	} else {
		playback_stop();
	}
	pending = PENDING_PLAYBACK;
	return 1;
}

EXPORT void playback_stop(void)
{
	if (pending == PENDING_PLAYBACK) {
		pending = PENDING_NONE;
	}
	if (playing) {
		stopping = 1;
		input_set_source(NULL);
		stopping = 0;
	}
}

//mode in the top 2 bits (0 idle, 1 recording, 2 playing), then the current frame,
//or when idle the length of the current recording
EXPORT uint32_t record_status(void)
{
	if (recording_active) {
		return 1U << 30 | (rec_frames & 0x3FFFFFFF);
	}
	if (playing) {
		return 2U << 30 | (play_frame & 0x3FFFFFFF);
	}
	return recording ? view.frames & 0x3FFFFFFF : 0;
}
