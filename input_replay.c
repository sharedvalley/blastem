//Records gamepad input and plays it back. Recording is a tap on the input applied
//each frame (input.h), playback an input source, so neither needs to know where
//input comes from, and the live, recorded and replayed paths are the same code.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "input_replay.h"
#include "input.h"
#include "io.h"
#include "quote.h"
#include "rom_coverage.h"
#include "serialize.h"
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
#define RECORD_VERSION 4
//version 2 checkpoints covered the whole state, so they can't be compared with version 3's
#define FIRST_CHECKPOINTED_VERSION 3
//version 4 records which build made it
#define FIRST_BUILD_TAGGED_VERSION 4
//The web and native builds don't yet replay each other's recordings identically, so a
//recording names the build that made it and only plays back on the same kind of build.
//Once they match, both will write "BECO" (common).
#ifdef __EMSCRIPTEN__
#define RECORD_BUILD "BEWE"
#else
#define RECORD_BUILD "BENA"
#endif
//recordings from before builds were tagged all came from the web build
#define UNTAGGED_BUILD "BEWE"
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
static system_header *play_system;
static pad_state play_state;
static uint32_t play_frame, play_next_event;
static uint8_t reported_desync;

//recordings coming from JS
static uint8_t *exchange;
static size_t exchange_size;

enum {
	PENDING_NONE,
	PENDING_RECORD,
	PENDING_PASS,
	PENDING_PLAYBACK
};
static uint8_t pending;

//the web build tells JS instead, through Module.onBlastemPlaybackEnd
static input_replay_end_handler end_handler;

void input_replay_on_playback_end(input_replay_end_handler handler)
{
	end_handler = handler;
}

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
static void notify_playback_end(void)
{
	if (end_handler) {
		end_handler();
	}
}
//the desync itself is already reported as a warning
static void notify_desync(uint32_t frame) {}
#endif

//Video and sound chip internals, e.g. line fetch buffers and operator phases, don't
//survive a save state round trip exactly, and the game can't observe them, so a
//checkpoint covers the CPUs, memories and the rest of the state they can see
static uint8_t checkpointed_section(uint16_t id)
{
	switch (id)
	{
	case SECTION_VDP:
	case SECTION_YM2612:
	case SECTION_PSG:
	case SECTION_RF5C164:
	case SECTION_CDD_FADER:
		return 0;
	default:
		return 1;
	}
}

static uint32_t state_crc(system_header *system)
{
	size_t size;
	uint8_t *state = system->serialize(system, &size);
	uint32_t crc = crc32(0, NULL, 0);
	//sections are a big endian 16-bit id and 32-bit size, then the data
	for (size_t offset = 0; size - offset >= 6;)
	{
		uint16_t id = state[offset] << 8 | state[offset + 1];
		uint32_t section_size = (uint32_t)state[offset + 2] << 24 | state[offset + 3] << 16
			| state[offset + 4] << 8 | state[offset + 5];
		if (section_size > size - offset - 6) {
			break;
		}
		if (checkpointed_section(id)) {
			crc = crc32(crc, state + offset, 6 + section_size);
		}
		offset += 6 + section_size;
	}
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
	rom_coverage_stop();
	if (!stopping) {
		//played to the end, so it read what its recording did
		quote_add_pass(recording, recording_size, view.state, view.state_size, rom_coverage_bitmap(COVERAGE_READ));
#ifndef __EMSCRIPTEN__
		//the state it ends in, to compare runs, e.g. of a quote and of the whole ROM
		if (play_system && play_system->serialize) {
			printf("Playback finished in state %08X\n", state_crc(play_system));
		}
#endif
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

//"BREC" | version | build | frames | pad state at the start | state size | state
//| event count | events | checkpoint count | checkpoints
//build is 4 characters (RECORD_BUILD), from version 4 on
//all values are little endian, as wasm is
static uint8_t parse_recording(uint8_t *data, size_t size, recording_view *out)
{
	uint8_t *cur = data, *end = data + size;
	uint32_t version;
	if (size < 4 || memcmp(cur, RECORD_MAGIC, 4)) {
		return 0;
	}
	cur += 4;
	if (!get32(&cur, end, &version) || version < 2 || version > RECORD_VERSION) {
		return 0;
	}
	const char *build = UNTAGGED_BUILD;
	if (version >= FIRST_BUILD_TAGGED_VERSION) {
		if (end - cur < 4) {
			return 0;
		}
		build = (const char *)cur;
		cur += 4;
	}
	if (memcmp(build, RECORD_BUILD, 4) && memcmp(build, "BECO", 4)) {
		warning("This recording was made by another kind of BlastEm build (%.4s, this is %s), which doesn't replay it identically yet\n", build, RECORD_BUILD);
		return 0;
	}
	if (!get32(&cur, end, &out->frames) || end - cur < PAD_STATE_BYTES) {
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
	if (version < FIRST_CHECKPOINTED_VERSION) {
		out->num_checkpoints = 0;
	}
	return 1;
}

//packs the recording in progress into the one buffer a finished recording lives in
static void finish_recording(void)
{
	size_t size = 4 + 4 + 4 + 4 + PAD_STATE_BYTES + 4 + rec_state_size + 4 + rec_num_events * sizeof(input_event)
		+ 4 + rec_num_checkpoints * sizeof(uint32_t);
	uint8_t *data = malloc(size), *cur = data;
	memcpy(cur, RECORD_MAGIC, 4);
	cur = put32(cur + 4, RECORD_VERSION);
	memcpy(cur, RECORD_BUILD, 4);
	cur = put32(cur + 4, rec_frames);
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

#ifndef __EMSCRIPTEN__
//Natively a system runs until asked to return, unlike the web build where it returns
//every frame, so a pending recording or playback asks it to at the end of a frame
static void boundary_tap(void *data, system_header *system, const pad_state *applied)
{
	input_remove_tap(boundary_tap, NULL);
	system_request_exit(system, 0);
}
#endif

static void set_pending(uint8_t new_pending)
{
	pending = new_pending;
#ifndef __EMSCRIPTEN__
	input_remove_tap(boundary_tap, NULL);
	if (pending) {
		input_add_tap(boundary_tap, NULL);
	}
#endif
}

uint8_t input_replay_pending(void)
{
	return pending != PENDING_NONE;
}

//state is the system's current state, which the recording takes over
static void start_recording(uint8_t *state, size_t state_size)
{
	free(rec_state);
	rec_state = state;
	rec_state_size = state_size;
	rec_initial = rec_last = *input_applied();
	rec_num_events = rec_num_checkpoints = rec_frames = 0;
	input_remove_tap(record_tap, NULL);
	input_add_tap(record_tap, NULL);
	rom_coverage_start();
	recording_active = 1;
}

//plays the current recording, with the system at its start state
static void begin_playback(system_header *system)
{
	//the loaded state has the recording's pads down, not the ones applied before
	input_reset_applied(&view.initial);
	quote_rearm();
	play_system = system;
	play_state = view.initial;
	play_frame = play_next_event = 0;
	reported_desync = 0;
	input_add_tap(verify_tap, NULL);
	input_set_source(&replay_source);
	rom_coverage_start();
	playing = 1;
}

void input_replay_play_started(system_header *system)
{
	if (!recording) {
		return;
	}
	begin_playback(system);
	//A system that hasn't run a frame yet differs from one between frames in VDP
	//timing state that states don't keep, which moves when input arrives. So the first
	//frame plays the recording, and its state is loaded again after it, as for any
	//other playback.
	set_pending(PENDING_PLAYBACK);
}

void input_replay_run_boundary(system_header *system)
{
	if (!pending || !system->serialize || !system->deserialize) {
		return;
	}
	if (pending == PENDING_RECORD) {
		size_t size;
		uint8_t *state = system->serialize(system, &size);
		start_recording(state, size);
	} else if (pending == PENDING_PASS) {
		system->deserialize(system, view.state, view.state_size);
		input_reset_applied(&view.initial);
		quote_rearm();
		//the state itself rather than serializing it again, which isn't exactly the same
		uint8_t *state = malloc(view.state_size);
		memcpy(state, view.state, view.state_size);
		start_recording(state, view.state_size);
	} else {
		if (playing) {
			//restarting it, so it didn't finish
			stopping = 1;
			input_set_source(NULL);
			stopping = 0;
		}
		system->deserialize(system, view.state, view.state_size);
		begin_playback(system);
	}
	set_pending(PENDING_NONE);
}

EXPORT void record_start(void)
{
	playback_stop();
	set_pending(PENDING_RECORD);
}

//records another pass from the start of the current recording, so several passes can
//cover more of a playable quote, returns 0 without a recording to start from
EXPORT uint8_t record_pass_start(void)
{
	record_stop();
	if (!recording) {
		return 0;
	}
	playback_stop();
	set_pending(PENDING_PASS);
	return 1;
}

EXPORT void record_stop(void)
{
	if (pending == PENDING_RECORD || pending == PENDING_PASS) {
		set_pending(PENDING_NONE);
	}
	if (recording_active) {
		recording_active = 0;
		input_remove_tap(record_tap, NULL);
		rom_coverage_stop();
		finish_recording();
		quote_add_pass(recording, recording_size, view.state, view.state_size, rom_coverage_bitmap(COVERAGE_READ));
	}
}

//the start state of the current recording
const uint8_t *input_replay_start_state(size_t *size)
{
	*size = recording ? view.state_size : 0;
	return recording ? view.state : NULL;
}

static uint8_t *built_quote;
static size_t built_quote_size;

//builds a playable quote from the passes recorded or played to the end since the last
//new start and returns its size, 0 if there are none. Its replay is the recording of
//official_size bytes JS copied into playback_buffer, or the first pass when 0.
EXPORT size_t quote_build_data(size_t official_size)
{
	free(built_quote);
	built_quote = NULL;
	if (official_size) {
		recording_view official;
		if (official_size > exchange_size || !parse_recording(exchange, official_size, &official)) {
			return 0;
		}
		built_quote = quote_build(exchange, official_size, official.state, official.state_size, NULL, &built_quote_size);
	} else {
		built_quote = quote_build(NULL, 0, NULL, 0, NULL, &built_quote_size);
	}
	return built_quote ? built_quote_size : 0;
}

//the quote quote_build_data built
EXPORT uint8_t *quote_data(void)
{
	return built_quote;
}

EXPORT uint32_t quote_pass_count(void)
{
	return quote_passes();
}

//the ROM bytes the last recording or playback read, one bit per byte of the ROM file,
//kind is COVERAGE_READ (any read) or COVERAGE_FETCH (68K instruction words)
EXPORT const uint8_t *record_coverage(uint8_t kind)
{
	return kind < COVERAGE_KINDS ? rom_coverage_bitmap(kind) : NULL;
}

EXPORT size_t record_coverage_size(void)
{
	return rom_coverage_bitmap_size();
}

EXPORT uint32_t record_rom_size(void)
{
	return rom_coverage_rom_size();
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
	set_pending(PENDING_PLAYBACK);
	return 1;
}

EXPORT void playback_stop(void)
{
	if (pending == PENDING_PLAYBACK) {
		set_pending(PENDING_NONE);
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

uint8_t input_replay_save(const char *path)
{
	if (!recording) {
		return 0;
	}
	FILE *f = fopen(path, "wb");
	if (!f) {
		return 0;
	}
	uint8_t written = fwrite(recording, 1, recording_size, f) == recording_size;
	return !fclose(f) && written;
}

//gzip("BLREC1\n" + JSON header + "\n" + recording), the website's format
#define WEB_MAGIC "BLREC1\n"

uint8_t input_replay_load(const char *path)
{
	//gzread reads files that aren't compressed as they are
	gzFile f = gzopen(path, "rb");
	if (!f) {
		return 0;
	}
	size_t size = 0, storage = 64 * 1024;
	uint8_t *data = malloc(storage);
	int read;
	while ((read = gzread(f, data + size, storage - size)) > 0)
	{
		size += read;
		if (size == storage) {
			storage *= 2;
			data = realloc(data, storage);
		}
	}
	gzclose(f);
	uint8_t *start = data;
	size_t magic_size = strlen(WEB_MAGIC);
	if (read == 0 && size > magic_size && !memcmp(data, WEB_MAGIC, magic_size)) {
		uint8_t *header_end = memchr(data + magic_size, '\n', size - magic_size);
		start = header_end ? header_end + 1 : NULL;
	}
	if (read < 0 || !start) {
		free(data);
		return 0;
	}
	size -= start - data;
	memcpy(playback_buffer(size), start, size);
	free(data);
	return playback_start(size);
}
