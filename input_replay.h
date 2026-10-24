#ifndef INPUT_REPLAY_H_
#define INPUT_REPLAY_H_

#include <stdint.h>
#include <stddef.h>
#include "system.h"

//Recording and playback of gamepad input, built on input.h: recording is a tap on
//the applied input, playback is an input source. A recording holds the save state
//taken when it started, so it can start anywhere in a game.

//starts and loads save states, which is only safe between runs of the system
void input_replay_run_boundary(system_header *system);
//whether a recording or playback waits for input_replay_run_boundary
uint8_t input_replay_pending(void);
//writes the current recording to a file
uint8_t input_replay_save(const char *path);
//plays a recording from a file, as written by input_replay_save or a .blrec from the
//website, returns 0 if it can't be read
uint8_t input_replay_load(const char *path);
//the start state of the current recording, NULL without one
const uint8_t *input_replay_start_state(size_t *size);
//plays the current recording from the system's first frame, for a system that starts
//from the recording's start state rather than loading it between runs
void input_replay_play_started(system_header *system);
typedef void (*input_replay_end_handler)(void);
//called when a playback reaches the end of its recording in native builds; the web
//build calls Module.onBlastemPlaybackEnd instead
void input_replay_on_playback_end(input_replay_end_handler handler);

//JS API (exported with EMSCRIPTEN_KEEPALIVE)
void record_start(void);
uint8_t record_pass_start(void);
void record_stop(void);
const uint8_t *record_coverage(uint8_t kind);
size_t record_coverage_size(void);
uint32_t record_rom_size(void);
uint8_t *record_data(void);
size_t record_data_size(void);
uint8_t *playback_buffer(size_t size);
uint8_t playback_start(size_t size);
void playback_stop(void);
uint32_t record_status(void);
size_t quote_build_data(size_t official_size);
uint8_t *quote_data(void);
uint32_t quote_pass_count(void);

#endif //INPUT_REPLAY_H_
