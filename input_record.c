//Records gamepad input as a save state plus the input changes of each frame, and
//plays such a recording back.
//The standalone frontend applies input whenever the game happens to read a pad
//after the host delivered an event, so the same presses would land on different
//cycles each run. While recording or playing, input is instead latched once per
//frame, at the end of the frame, which only depends on emulated state. Recording
//and playback go through the same latch, so a playback matches its recording.
#include <stdlib.h>
#include <string.h>
#include "input_record.h"
#include "zlib/zlib.h"
#include "util.h"
#include "blastem.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

#define RECORD_MAGIC "BREC"
#define RECORD_VERSION 1
#define CHECKPOINT_FRAMES 60
#define MAX_PENDING 64

typedef struct {
	uint32_t frame;
	uint8_t  pad;
	uint8_t  button;
	uint8_t  down;
	uint8_t  unused;
} input_event;

enum {
	PENDING_NONE,
	PENDING_RECORD,
	PENDING_PLAYBACK
};

static uint8_t mode;
static uint8_t pending;
static uint32_t frame;
//total frames of the recording, only meaningful once recording stopped or while playing
static uint32_t num_frames;

static input_event queued[MAX_PENDING];
static uint32_t num_queued;

static input_event *events;
static uint32_t num_events, events_storage;
//index of the next event to apply during playback
static uint32_t next_event;

static uint8_t *start_state;
static size_t start_state_size;

static uint32_t *checkpoints;
static uint32_t num_checkpoints, checkpoints_storage;
static uint8_t reported_desync;

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

static void apply(input_event *event)
{
	if (!current_system) {
		return;
	}
	if (event->down) {
		if (current_system->gamepad_down) {
			current_system->gamepad_down(current_system, event->pad, event->button);
		}
	} else if (current_system->gamepad_up) {
		current_system->gamepad_up(current_system, event->pad, event->button);
	}
}

static void log_event(input_event *event)
{
	if (num_events == events_storage) {
		events_storage = events_storage ? events_storage * 2 : 1024;
		events = realloc(events, events_storage * sizeof(input_event));
	}
	events[num_events++] = *event;
}

static uint32_t state_crc(void)
{
	size_t size;
	uint8_t *state = current_system->serialize(current_system, &size);
	uint32_t crc = crc32(0, state, size);
	free(state);
	return crc;
}

void input_record_gamepad(uint8_t down, uint8_t pad, uint8_t button)
{
	if (mode == INPUT_RECORD_OFF) {
		input_event event = {.pad = pad, .button = button, .down = down};
		apply(&event);
	} else if (mode == INPUT_RECORD_RECORDING && num_queued < MAX_PENDING) {
		queued[num_queued++] = (input_event){.pad = pad, .button = button, .down = down};
	}
	//live input is ignored during playback
}

void input_record_frame_end(void)
{
	if (mode == INPUT_RECORD_RECORDING) {
		for (uint32_t i = 0; i < num_queued; i++)
		{
			queued[i].frame = frame;
			apply(queued + i);
			log_event(queued + i);
		}
		num_queued = 0;
	} else if (mode == INPUT_RECORD_PLAYING) {
		while (next_event < num_events && events[next_event].frame == frame)
		{
			apply(events + next_event++);
		}
	} else {
		return;
	}
	frame++;
	if (frame % CHECKPOINT_FRAMES == 0 && current_system->serialize) {
		uint32_t crc = state_crc();
		uint32_t index = frame / CHECKPOINT_FRAMES - 1;
		if (mode == INPUT_RECORD_RECORDING) {
			if (num_checkpoints == checkpoints_storage) {
				checkpoints_storage = checkpoints_storage ? checkpoints_storage * 2 : 256;
				checkpoints = realloc(checkpoints, checkpoints_storage * sizeof(uint32_t));
			}
			checkpoints[num_checkpoints++] = crc;
		} else if (index < num_checkpoints && checkpoints[index] != crc && !reported_desync) {
			reported_desync = 1;
			warning("Playback desynced from its recording by frame %u\n", frame);
			notify_desync(frame);
		}
	}
	if (mode == INPUT_RECORD_PLAYING && frame >= num_frames) {
		mode = INPUT_RECORD_OFF;
		notify_playback_end();
	}
}

void input_record_run_boundary(system_header *system)
{
	if (!pending || !system->serialize || !system->deserialize) {
		return;
	}
	if (pending == PENDING_RECORD) {
		free(start_state);
		start_state = system->serialize(system, &start_state_size);
		num_events = num_checkpoints = num_queued = 0;
		num_frames = frame = 0;
		mode = INPUT_RECORD_RECORDING;
	} else {
		system->deserialize(system, start_state, start_state_size);
		frame = next_event = 0;
		reported_desync = 0;
		mode = INPUT_RECORD_PLAYING;
	}
	pending = PENDING_NONE;
}

EXPORT void record_start(void)
{
	mode = INPUT_RECORD_OFF;
	pending = PENDING_RECORD;
}

EXPORT void record_stop(void)
{
	if (mode == INPUT_RECORD_RECORDING) {
		num_frames = frame;
		mode = INPUT_RECORD_OFF;
	}
	if (pending == PENDING_RECORD) {
		pending = PENDING_NONE;
	}
}

static uint8_t *put32(uint8_t *dst, uint32_t value)
{
	memcpy(dst, &value, sizeof(value));
	return dst + sizeof(value);
}

//"BREC" | version | frames | state size | state | event count | events | checkpoint count | checkpoints
//all values are little endian, as wasm is
EXPORT uint8_t *record_data(size_t *size_out)
{
	if (!start_state || mode == INPUT_RECORD_RECORDING) {
		return NULL;
	}
	size_t size = 4 + 4 * 3 + start_state_size + 4 + num_events * sizeof(input_event) + 4 + num_checkpoints * sizeof(uint32_t);
	uint8_t *data = malloc(size), *cur = data;
	memcpy(cur, RECORD_MAGIC, 4);
	cur = put32(cur + 4, RECORD_VERSION);
	cur = put32(cur, num_frames);
	cur = put32(cur, start_state_size);
	memcpy(cur, start_state, start_state_size);
	cur = put32(cur + start_state_size, num_events);
	memcpy(cur, events, num_events * sizeof(input_event));
	cur = put32(cur + num_events * sizeof(input_event), num_checkpoints);
	memcpy(cur, checkpoints, num_checkpoints * sizeof(uint32_t));
	*size_out = size;
	return data;
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

//loads a recording made by record_data, or replays the one in memory when data is NULL
EXPORT uint8_t playback_start(uint8_t *data, size_t size)
{
	if (data) {
		uint8_t *cur = data, *end = data + size;
		uint32_t version, frames, state_size, count, crc_count;
		if (size < 4 || memcmp(cur, RECORD_MAGIC, 4)) {
			return 0;
		}
		cur += 4;
		if (!get32(&cur, end, &version) || version != RECORD_VERSION || !get32(&cur, end, &frames)
			|| !get32(&cur, end, &state_size) || end - cur < state_size) {
			return 0;
		}
		uint8_t *state = cur;
		cur += state_size;
		if (!get32(&cur, end, &count) || (end - cur) / sizeof(input_event) < count) {
			return 0;
		}
		uint8_t *event_data = cur;
		cur += count * sizeof(input_event);
		if (!get32(&cur, end, &crc_count) || (end - cur) / sizeof(uint32_t) < crc_count) {
			return 0;
		}
		mode = INPUT_RECORD_OFF;
		free(start_state);
		start_state = malloc(state_size);
		memcpy(start_state, state, state_size);
		start_state_size = state_size;
		events = realloc(events, (count ? count : 1) * sizeof(input_event));
		memcpy(events, event_data, count * sizeof(input_event));
		num_events = events_storage = count;
		checkpoints = realloc(checkpoints, (crc_count ? crc_count : 1) * sizeof(uint32_t));
		memcpy(checkpoints, cur, crc_count * sizeof(uint32_t));
		num_checkpoints = checkpoints_storage = crc_count;
		num_frames = frames;
	} else if (!start_state || mode == INPUT_RECORD_RECORDING) {
		return 0;
	}
	mode = INPUT_RECORD_OFF;
	pending = PENDING_PLAYBACK;
	return 1;
}

EXPORT void playback_stop(void)
{
	if (mode == INPUT_RECORD_PLAYING) {
		mode = INPUT_RECORD_OFF;
	}
	if (pending == PENDING_PLAYBACK) {
		pending = PENDING_NONE;
	}
}

//mode in the top 2 bits, current frame in the rest
EXPORT uint32_t record_status(void)
{
	return (uint32_t)mode << 30 | (frame & 0x3FFFFFFF);
}
