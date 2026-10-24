/*
 Copyright 2013-2016 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifndef USE_FBDEV
// for SDL_main on platforms that need it
#include <SDL.h>
#endif

#include "system.h"
#include "68kinst.h"
#include "mem.h"
#include "vdp.h"
#include "render.h"
#include "genesis.h"
#include "gdb_remote.h"
#include "gst.h"
#include "util.h"
#include "paths.h"
#include "romdb.h"
#include "terminal.h"
#include "arena.h"
#include "config.h"
#include "bindings.h"
#include "menu.h"
#include "zip.h"
#include "cdimage.h"
#include "event_log.h"
#include "input_replay.h"
#include "quote.h"
#ifndef DISABLE_NUKLEAR
#include "nuklear_ui/blastem_nuklear.h"
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include "render_audio.h"
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

#include "version.inc"

#ifdef __ANDROID__
#define FULLSCREEN_DEFAULT 1
#else
#define FULLSCREEN_DEFAULT 0
#endif

int headless = 0;
int exit_after = 0;
int z80_enabled = 1;
int frame_limit = 0;
uint8_t use_native_states = 1;

tern_node * config;

int break_on_sync = 0;
char *save_state_path;
char * save_filename;
system_header *current_system;
system_header *menu_system;
system_header *game_system;
void persist_save()
{
	if (!game_system || !game_system->persist_save) {
		return;
	}
	game_system->persist_save(game_system);
}

char *title;
void update_title(char *rom_name)
{
	if (title) {
		free(title);
		title = NULL;
	}
	title = alloc_concat(rom_name, " - BlastEm");
	render_update_caption(title);
}

static char *get_save_dir(system_media *media)
{
	char *savedir_template = tern_find_path(config, "ui\0save_path\0", TVAL_PTR).ptrval;
	if (!savedir_template) {
		savedir_template = "$USERDATA/blastem/$ROMNAME";
	}
	tern_node *vars = tern_insert_ptr(NULL, "ROMNAME", media->name);
	vars = tern_insert_ptr(vars, "ROMDIR", media->dir);
	vars = tern_insert_ptr(vars, "HOME", get_home_dir());
	vars = tern_insert_ptr(vars, "EXEDIR", get_exe_dir());
	vars = tern_insert_ptr(vars, "USERDATA", (char *)get_userdata_dir());
	char *save_dir = replace_vars(savedir_template, vars, 1);
	tern_free(vars);
	if (!ensure_dir_exists(save_dir)) {
		warning("Failed to create save directory %s\n", save_dir);
	}
	return save_dir;
}

const char *get_save_fname(uint8_t save_type)
{
	switch(save_type)
	{
	case SAVE_I2C: return "save.eeprom";
	case SAVE_NOR: return "save.nor";
	case SAVE_HBPT: return "save.hbpt";
	default: return "save.sram";
	}
}

void setup_saves(system_media *media, system_header *context)
{
	if (!context->load_save) {
		// system doesn't support saves
		return;
	}
	static uint8_t persist_save_registered;
	rom_info *info = &context->info;
	char *save_dir = get_save_dir(info->is_save_lock_on ? media->chain : media);
	char const *parts[] = {save_dir, PATH_SEP, get_save_fname(info->save_type)};
	free(save_filename);
	save_filename = alloc_concat_m(3, parts);
	if (info->is_save_lock_on) {
		//initial save dir was calculated based on lock-on cartridge because that's where the save device is
		//save directory used for save states should still be located in the normal place
		free(save_dir);
		parts[0] = save_dir = get_save_dir(media);
	}
	if (use_native_states || context->type != SYSTEM_GENESIS) {
		parts[2] = "quicksave.state";
	} else {
		parts[2] = "quicksave.gst";
	}
	free(save_state_path);
	save_state_path = alloc_concat_m(3, parts);
	context->save_dir = save_dir;
	if (info->save_type != SAVE_NONE || context->type == SYSTEM_SEGACD || context->type == SYSTEM_32XCD
		|| (context->type == SYSTEM_GENESIS && info->wants_cd)
	) {
		context->load_save(context);
		if (!persist_save_registered) {
			atexit(persist_save);
			persist_save_registered = 1;
		}
	}
}

static uint8_t menu;
static uint8_t use_nuklear;
#ifdef __EMSCRIPTEN__
void handle_frame_presented(void)
{
	if (current_system) {
		current_system->request_exit(current_system);
	}
}

void browser_main_loop(void)
{
	static uint8_t system_started;
#ifndef DISABLE_NUKLEAR
	static uint8_t was_menu;
	if (use_nuklear) {
		if (menu) {
			if (!was_menu) {
				ui_enter();
			}
			was_menu = menu;
			render_update_display();
			menu = !get_content_binding_state();
			if (!menu) {
				ui_exit();
				return;
			}
		} else {
			was_menu = menu;
		}
	}
#endif
	if (!current_system && game_system) {
		current_system = game_system;
	}
	if (current_system) {
		if (system_started && !current_system->force_release && render_is_audio_sync()) {
			if (all_sources_ready()) {
				return;
			}
		}
		if (current_system->next_rom) {
			char *next_rom = current_system->next_rom;
			current_system->next_rom = NULL;
			init_system_with_media(next_rom, 0);
			system_started = 0;
			menu = 0;
			current_system = game_system;
		} else if (!menu) {
			if (current_system->paused) {
				//e.g. after leaving a playable quote, until the page plays or records from
				//its start again
				if (!input_replay_pending()) {
					return;
				}
				current_system->paused = 0;
			}
			if (system_started) {
				input_replay_run_boundary(current_system);
				current_system->resume_context(current_system);
			} else {
				system_started = 1;
				current_system->start_context(current_system, NULL);
			}
			if (current_system->force_release) {
				menu = 1;
			}
		}
	}
	
}

void setup_main_loop(void)
{
	//can't use render_is_audio_sync since we haven't called render_init/render_config_updated yet
	char *sync = tern_find_path_default(config, "system\0sync_source\0", (tern_val){.ptrval = "audio"}, TVAL_PTR).ptrval;
	emscripten_cancel_main_loop();
	if (!strcmp("video", sync)) {
		render_set_audio_full_fun(NULL);
		render_set_frame_presented_fun(handle_frame_presented);
		emscripten_set_main_loop(browser_main_loop, 0, 0);
	} else {
		render_set_frame_presented_fun(NULL);
		render_set_audio_full_fun(handle_frame_presented);
		emscripten_set_main_loop(browser_main_loop, 1, 0); //dummy fps value, will be overridden by a call to emscripten_set_main_loop_timing
	}
}
#endif

void apply_updated_config(void)
{
#ifdef __EMSCRIPTEN__
	setup_main_loop();
#endif
	render_config_updated();
	set_bindings();
	update_pad_bindings();
	if (current_system && current_system->config_updated) {
		current_system->config_updated(current_system);
	}
}

static system_media cart, lock_on;
static void on_drag_drop(char *filename)
{
	if (current_system) {
		if (current_system->next_rom) {
			free(current_system->next_rom);
		}
		current_system->next_rom = strdup(filename);
		system_request_exit(current_system, 1);
		if (menu_system && menu_system->type == SYSTEM_GENESIS) {
			genesis_context *gen = (genesis_context *)menu_system;
			if (gen->extra) {
				menu_context *menu = gen->extra;
				menu->external_game_load = 1;
			}
		}
		cart.chain = NULL;
	} else {
		init_system_with_media(filename, SYSTEM_UNKNOWN);
	}
#ifndef DISABLE_NUKLEAR
	if (is_nuklear_active()) {
		show_play_view();
	}
#endif
}

const system_media *current_media(void)
{
	return &cart;
}

void reload_media(void)
{
	if (!current_system || !cart.orig_path) {
		return;
	}
	if (current_system->next_rom) {
		free(current_system->next_rom);
	}
	current_system->next_rom = cart.orig_path;
	cart.orig_path = NULL;
	if (cart.chain) {
		load_media(cart.chain->orig_path, cart.chain, NULL);
	}
	system_request_exit(current_system, 1);
}

void lockon_media(char *lock_on_path)
{
	free(lock_on.dir);
	free(lock_on.name);
	free(lock_on.extension);
	free(lock_on.orig_path);
	if (lock_on_path) {
		if (!current_system || !current_system->lockon_change) {
			cart.chain = NULL;
			reload_media();
		}
		cart.chain = &lock_on;
		load_media(lock_on_path, &lock_on, NULL);
		if (current_system && current_system->lockon_change) {
			current_system->lockon_change(current_system, &lock_on);
		}
	} else {
		lock_on.dir = NULL;
		lock_on.name = NULL;
		lock_on.extension = NULL;
		lock_on.orig_path = NULL;
		cart.chain = NULL;
	}
}

static uint32_t opts = 0;
static uint8_t force_region = 0;
#ifdef __EMSCRIPTEN__
EM_JS(void, notify_quote_miss, (uint32_t offset, uint32_t address), {
	if (Module.onBlastemQuoteMiss) {
		Module.onBlastemQuoteMiss(offset, address);
	}
});
#endif

static void quote_missed(uint32_t offset, uint32_t address)
{
	printf("Left the playable quote: the game ran code at $%06X (ROM offset $%06X), which the quote doesn't keep\n", address, offset);
	fflush(stdout);
	if (headless) {
		//for scripts, which can't unpause
		exit(3);
	}
	current_system->paused = 1;
#ifdef __EMSCRIPTEN__
	notify_quote_miss(offset, address);
	//the browser loop takes a forced release for a request to open the menu
	system_request_exit(current_system, 0);
#else
	system_request_exit(current_system, 1);
#endif
}

//A playable quote only has the ROM it needs from its start state on, so it can't boot.
//It starts at that state instead, playing its official recording, and the game stops
//if it leaves the quote.
static void start_quote(system_header *system)
{
	if (!quote_loaded(cart.buffer)) {
		return;
	}
	size_t size;
	const uint8_t *replay = quote_replay(&size);
	memcpy(playback_buffer(size), replay, size);
	if (!playback_start(size)) {
		fatal_error("The recording in this playable quote can't be played\n");
	}
	//a copy, as the current recording changes when another is made or loaded
	const uint8_t *state = input_replay_start_state(&system->start_state_size);
	system->start_state = malloc(system->start_state_size);
	memcpy(system->start_state, state, system->start_state_size);
	//from the first frame: a frame of live input before it could leave the quote
	input_replay_play_started(system);
	quote_check(quote_missed);
}

void init_system_with_media(char *path, system_type force_stype)
{
	if (game_system) {
		if (game_system->persist_save) {
			game_system->persist_save(game_system);
		}
		//swap to game context arena and mark all allocated pages in it free
		if (current_system == menu_system) {
			current_system->arena = set_current_arena(game_system->arena);
		}
		mark_all_free();
		game_system->free_context(game_system);
	} else if(current_system) {
		//start a new arena and save old one in suspended system context
		current_system->arena = start_new_arena();
	}
	free(cart.dir);
	free(cart.name);
	free(cart.extension);
	free(cart.orig_path);
	system_type stype = SYSTEM_UNKNOWN;
	if (!(cart.size = load_media(path, &cart, &stype))) {
		fatal_error("Failed to open %s for reading\n", path);
	}

	if (force_stype != SYSTEM_UNKNOWN) {
		stype = force_stype;
	}
	if (stype == SYSTEM_UNKNOWN) {
		stype = detect_system_type(&cart);
	}
	if (stype == SYSTEM_UNKNOWN) {
		fatal_error("Failed to detect system type for %s\n", path);
	}
	//allocate new system context
	game_system = alloc_config_system(stype, &cart, opts, force_region);
	if (!game_system) {
		fatal_error("Failed to configure emulated machine for %s\n", path);
	}
	if (menu_system) {
		menu_system->next_context = game_system;
	}
	game_system->next_context = menu_system;
	setup_saves(&cart, game_system);
	update_title(game_system->info.name);
	start_quote(game_system);
}

void render_log_handler(log_level level, char *message)
{
	switch(level)
	{
	case INFO:
		render_infobox("BlastEm Info", message);
		break;
	case WARN:
		render_warnbox("BlastEm Warning", message);
		break;
	case FATAL:
		render_errorbox("Fatal Error", message);
		break;
	default:
		break;
	}
}

char *parse_addr_port(char *arg)
{
	while (*arg && *arg != ':') {
		++arg;
	}
	if (!*arg) {
		return NULL;
	}
	char *end;
	int port = strtol(arg + 1, &end, 10);
	if (port && !*end) {
		*arg = 0;
		return arg + 1;
	}
	return NULL;
}

//-R takes a comma separated list, played one after the other
static char **replay_paths;
static int num_replay_paths, next_replay;
static char *quote_path;
static uint8_t quote_written;

static void write_quote(void)
{
	if (!quote_written) {
		quote_written = 1;
		quote_save(game_system ? game_system->info.name : NULL, quote_path);
	}
}

static char *record_path;

//plays the next recording given to -R. Once all played, it starts -W's recording, so
//that one starts where they left the game, or writes -Q's quote and exits.
static void play_next_recording(void)
{
	if (next_replay < num_replay_paths) {
		char *path = replay_paths[next_replay++];
		if (!input_replay_load(path)) {
			fatal_error("Failed to load the recording %s\n", path);
		}
	} else if (record_path) {
		record_start();
	} else if (quote_path) {
		write_quote();
		exit(0);
	}
}

static void save_recording(void)
{
	record_stop();
	if (input_replay_save(record_path)) {
		printf("Saved the recording to %s\n", record_path);
	} else {
		warning("Failed to save the recording to %s\n", record_path);
	}
}

int main(int argc, char ** argv)
{
	set_exe_str(argv[0]);
#ifndef _WIN32
	if (!(isatty(STDERR_FILENO) && isatty(STDIN_FILENO))) {
#endif
		register_log_handler(render_log_handler);
#ifndef _WIN32
	}
#endif
	
	config = load_config();
	int width = -1;
	int height = -1;
	int debug = 0;
	int loaded = 0;
	system_type stype = SYSTEM_UNKNOWN, force_stype = SYSTEM_UNKNOWN;
	char * romfname = NULL;
	char * statefile = NULL;
	char *replay_list = NULL;
	char *reader_addr = NULL, *reader_port = NULL;
	event_reader reader = {0};
	debugger_type dtype = DEBUGGER_NATIVE;
	uint8_t start_in_debugger = 0;
	uint8_t fullscreen = FULLSCREEN_DEFAULT, use_gl = 1;
	uint8_t debug_target = 0;
	char *port;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			switch(argv[i][1]) {
			case 'b':
				i++;
				if (i >= argc) {
					fatal_error("-b must be followed by a frame count\n");
				}
				register_log_handler(NULL);
				headless = 1;
				exit_after = atoi(argv[i]);
				break;
			case 'd':
				start_in_debugger = 1;
				//allow debugging the menu
				if (argv[i][2] == 'm') {
					debug_target = 1;
				}
				break;
			case 'D':
				gdb_remote_init();
				dtype = DEBUGGER_GDB;
				start_in_debugger = 1;
				break;
			case 'e':
				i++;
				if (i >= argc) {
					fatal_error("-e must be followed by a file name\n");
				}
				port = parse_addr_port(argv[i]);
				if (port) {
					event_log_tcp(argv[i], port);
				} else {
					event_log_file(argv[i]);
				}
				break;
			case 'f':
				fullscreen = !fullscreen;
				break;
			case 'g':
				use_gl = 0;
				break;
			case 'l':
				opts |= OPT_ADDRESS_LOG;
				break;
			case 'v':
				info_message("blastem %s\n", BLASTEM_VERSION);
				return 0;
				break;
			case 'n':
				z80_enabled = 0;
				break;
			case 'r':
				i++;
				if (i >= argc) {
					fatal_error("-r must be followed by region (J, U or E)\n");
				}
				force_region = translate_region_char(toupper(argv[i][0]));
				if (!force_region) {
					fatal_error("'%c' is not a valid region character for the -r option\n", argv[i][0]);
				}
				break;
			case 'm':
				i++;
				if (i >= argc) {
					fatal_error("-r must be followed by a machine type (sms, gg, sg, sc, gen, 32x, 32xcd, pico, copera, jag or media)\n");
				}
				if (!strcmp("sms", argv[i])) {
					stype = force_stype = SYSTEM_SMS;
				} else if (!strcmp("gg", argv[i])) {
					stype = force_stype = SYSTEM_GAME_GEAR;
				} else if (!strcmp("sg", argv[i])) {
					stype = force_stype = SYSTEM_SG1000;
				} else if (!strcmp("sc", argv[i])) {
					stype = force_stype = SYSTEM_SC3000;
				} else if (!strcmp("gen", argv[i])) {
					stype = force_stype = SYSTEM_GENESIS;
				} else if (!strcmp("32x", argv[i])) {
					stype = force_stype = SYSTEM_32X;
				} else if (!strcmp("32xcd", argv[i])) {
					stype = force_stype = SYSTEM_32XCD;
				} else if (!strcmp("pico", argv[i])) {
					stype = force_stype = SYSTEM_PICO;
				} else if (!strcmp("copera", argv[i])) {
					stype = force_stype = SYSTEM_COPERA;
				} else if (!strcmp("jag", argv[i])) {
					stype = force_stype = SYSTEM_JAGUAR;
				} else if (!strcmp("media", argv[i])) {
					stype = force_stype = SYSTEM_MEDIA_PLAYER;
				} else if (!strcmp("laser", argv[i])) {
					stype = force_stype = SYSTEM_LASERACTIVE;
				} else {
					fatal_error("Unrecognized machine type %s\n", argv[i]);
				}
				break;
			case 's':
				i++;
				if (i >= argc) {
					fatal_error("-s must be followed by a savestate filename\n");
				}
				statefile = argv[i];
				break;
			case 't':
				force_no_terminal();
				break;
			case 'R':
				i++;
				if (i >= argc) {
					fatal_error("-R must be followed by a recording filename\n");
				}
				replay_list = argv[i];
				break;
			case 'Q':
				i++;
				if (i >= argc) {
					fatal_error("-Q must be followed by a filename\n");
				}
				quote_path = argv[i];
				break;
			case 'W':
				i++;
				if (i >= argc) {
					fatal_error("-W must be followed by a recording filename\n");
				}
				record_path = argv[i];
				break;
			case 'y':
				opts |= YM_OPT_WAVE_LOG;
				break;
			case 'o': {
				i++;
				if (i >= argc) {
					fatal_error("-o must be followed by a lock on cartridge filename\n");
				}
				if (!load_media(argv[i], &lock_on, NULL)) {
					fatal_error("Failed to load lock on cartridge %s\n", argv[i]);
				}
				cart.chain = &lock_on;
				break;
			}
			case 'h':
				info_message(
					"Usage: blastem [OPTIONS] ROMFILE [WIDTH] [HEIGHT]\n"
					"Options:\n"
					"	-h          Print this help text\n"
					"	-r (J|U|E)  Force region to Japan, US or Europe respectively\n"
					"	-m MACHINE  Force emulated machine type to MACHINE. Valid values are:\n"
					"                   sms    - Sega Master System/Mark III\n"
					"                   gg     - Sega Game Gear\n"
					"                   sg     - Sega SG-1000\n"
					"                   sc     - Sega SC-3000\n"
					"                   gen    - Sega Genesis/Megadrive\n"
					"                   32x    - Sega 32X\n"
					"                   pico   - Sega Pico\n"
					"                   copera - Yamaha Copera\n"
					"                   media  - Media Player\n"
					"                   laser  - Pioneer LaserActive (no PAC)\n"
					"	-f          Toggles fullscreen mode\n"
					"	-g          Disable OpenGL rendering\n"
					"	-s FILE     Load a GST format savestate from FILE\n"
					"	-R FILES    Play the input recordings in FILES, separated by commas, one after\n"
					"	            the other once the game starts\n"
					"	-Q FILE     Write a playable quote of the recordings made or played to FILE:\n"
					"	            once the -R recordings played, otherwise on exit\n"
					"	-W FILE     Record inputs once the game starts, or once the -R recordings\n"
					"	            played, and write them to FILE on exit\n"
					"	-o FILE     Load FILE as a lock-on cartridge\n"
					"	-d          Enter debugger on startup\n"
					"	-n          Disable Z80\n"
					"	-v          Display version number and exit\n"
					"	-l          Log 68K code addresses (useful for assemblers)\n"
					"	-y          Log individual YM-2612 channels to WAVE files\n"
					"   -e FILE     Write hardware event log to FILE\n"
				);
				return 0;
			default:
				fatal_error("Unrecognized switch %s\n", argv[i]);
			}
		} else if (!loaded) {
			reader_port = parse_addr_port(argv[i]);
			if (reader_port) {
				reader_addr = argv[i];
			} else {
				romfname = strdup(argv[i]);
				if (!load_media(romfname, &cart, stype == SYSTEM_UNKNOWN ? &stype : NULL)) {
					fatal_error("Failed to open %s for reading\n", argv[i]);
				}
			}
			loaded = 1;
		} else if (width < 0) {
			width = atoi(argv[i]);
		} else if (height < 0) {
			height = atoi(argv[i]);
		}
	}

	int def_width = 0, def_height = 0;
	char *config_width = tern_find_path(config, "video\0width\0", TVAL_PTR).ptrval;
	if (config_width) {
		def_width = atoi(config_width);
	}
	if (!def_width) {
		def_width = 640;
	}
	char *config_height = tern_find_path(config, "video\0height\0", TVAL_PTR).ptrval;
	if (config_height) {
		def_height = atoi(config_height);
	}
	if (!def_height) {
		def_height = -1;
	}
	width = width < 1 ? def_width : width;
	height = height < 1 ? def_height : height;

	char *config_fullscreen = tern_find_path(config, "video\0fullscreen\0", TVAL_PTR).ptrval;
	if (config_fullscreen && !strcmp("on", config_fullscreen)) {
		fullscreen = !fullscreen;
	}
#ifdef __EMSCRIPTEN__
	config = tern_insert_path(config, "ui\0initial_path\0", (tern_val){.ptrval = strdup("/roms")}, TVAL_PTR);
	setup_main_loop();
#endif
	if (!headless) {
		if (reader_addr) {
			render_set_external_sync(1);
		}
		render_init(width, height, "BlastEm", fullscreen);
		render_set_drag_drop_handler(on_drag_drop);
	}
	set_bindings();
	menu = !loaded;

#ifndef DISABLE_NUKLEAR
	use_nuklear = !headless && is_nuklear_available();
#endif
	if (!loaded && !use_nuklear) {
		//load menu
		romfname = tern_find_path(config, "ui\0rom\0", TVAL_PTR).ptrval;
		if (!romfname) {
			romfname = "menu.bin";
		}
		romfname = strdup(romfname);
		if (is_absolute_path(romfname)) {
			if (!(cart.size = load_media(romfname, &cart, &stype))) {
				fatal_error("Failed to open UI ROM %s for reading", romfname);
			}
		} else {
			cart.buffer = (uint16_t *)read_bundled_file(romfname, &cart.size);
			if (!cart.buffer) {
				fatal_error("Failed to open UI ROM %s for reading", romfname);
			}
			uint32_t rom_size = nearest_pow2(cart.size);
			if (rom_size > cart.size) {
				cart.buffer = realloc(cart.buffer, rom_size);
				cart.size = rom_size;
			}
			cart.dir = path_dirname(romfname);
			cart.name = basename_no_extension(romfname);
			cart.extension = path_extension(romfname);
			cart.orig_path = romfname;
		}
		//force system detection, value on command line is only for games not the menu
		stype = detect_system_type(&cart);
		loaded = 1;
	}
	char *state_format = tern_find_path(config, "ui\0state_format\0", TVAL_PTR).ptrval;
	if (state_format && !strcmp(state_format, "gst")) {
		use_native_states = 0;
	} else if (state_format && strcmp(state_format, "native")) {
		warning("%s is not a valid value for the ui.state_format setting. Valid values are gst and native\n", state_format);
	}

	if (loaded && !reader_addr) {
		if (stype == SYSTEM_UNKNOWN) {
			stype = detect_system_type(&cart);
		}
		if (stype == SYSTEM_UNKNOWN) {
			fatal_error("Failed to detect system type for %s\n", romfname);
		}

		current_system = alloc_config_system(stype, &cart, menu ? 0 : opts, force_region);
		if (!current_system) {
			fatal_error("Failed to configure emulated machine for %s\n", romfname);
		}

		setup_saves(&cart, current_system);
		update_title(current_system->info.name);
		if (menu) {
			menu_system = current_system;
		} else {
			game_system = current_system;
			start_quote(game_system);
		}
	}

#ifndef DISABLE_NUKLEAR
	if (use_nuklear) {
		blastem_nuklear_init(!menu);
#ifndef __EMSCRIPTEN__
		current_system = game_system;
		menu = 0;
#endif
	}
#endif

	if (reader_addr) {
		init_event_reader_tcp(&reader, reader_addr, reader_port);
		stype = reader_system_type(&reader);
		if (stype == SYSTEM_UNKNOWN) {
			fatal_error("Failed to detect system type for %s\n", romfname);
		}
		game_system = current_system = alloc_config_player(stype, &reader);
		//free inflate stream as it was inflateCopied to an internal event reader in the player
		inflateEnd(&reader.input_stream);
		setup_saves(&cart, current_system);
		update_title(current_system->info.name);
	}

	current_system->debugger_type = dtype;
	current_system->enter_debugger = start_in_debugger && menu == debug_target;
	if (replay_list) {
		for (char *path = strtok(replay_list, ","); path; path = strtok(NULL, ","))
		{
			replay_paths = realloc(replay_paths, (num_replay_paths + 1) * sizeof(char *));
			replay_paths[num_replay_paths++] = path;
		}
		input_replay_on_playback_end(play_next_recording);
		play_next_recording();
	}
	if (quote_path) {
		atexit(write_quote);
	}
	if (record_path) {
		if (!replay_list) {
			record_start();
		}
		//-b ends with exit(), so the recording is written from an exit handler
		atexit(save_recording);
	}
#ifndef __EMSCRIPTEN__
	current_system->start_context(current_system,  menu ? NULL : statefile);
	render_video_loop();
	for(;;)
	{
		if (current_system->should_exit) {
			break;
		}
		uint8_t was_paused = current_system->paused;
		while (current_system->paused) {
			uint32_t start = render_elapsed_ms();
			process_events();
			render_update_display();
			if (current_system->get_vdp) {
				vdp_context *vdp = current_system->get_vdp(current_system);
				if (vdp) {
					vdp_update_per_frame_debug(vdp);
				}
			}
			uint32_t duration = render_elapsed_ms() - start;
			if (duration < 1000/60) {
				render_sleep_ms(1000/60 - duration);
			}
		}
		if (current_system->next_rom) {
			char *next_rom = current_system->next_rom;
			current_system->next_rom = NULL;
			init_system_with_media(next_rom, force_stype);
			menu = 0;
			current_system = game_system;
			current_system->debugger_type = dtype;
			current_system->enter_debugger = start_in_debugger && menu == debug_target;
			current_system->start_context(current_system, statefile);
			render_video_loop();
		} else if (was_paused) {
			current_system->resume_context(current_system);
		} else if (!menu && input_replay_pending()) {
			//a recording or playback asked the system to return so it can start
			input_replay_run_boundary(current_system);
			current_system->resume_context(current_system);
		} else if (menu && game_system) {
			current_system->arena = set_current_arena(game_system->arena);
			current_system = game_system;
			menu = 0;
			current_system->resume_context(current_system);
		} else if (!menu && (menu_system || use_nuklear)) {
			if (use_nuklear) {
#ifndef DISABLE_NUKLEAR
				ui_idle_loop();
#endif
			} else {
				current_system->arena = set_current_arena(menu_system->arena);
				current_system = menu_system;
				menu = 1;
			}
			if (!current_system->next_rom) {
				current_system->resume_context(current_system);
				render_video_loop();
			}
		} else {
			break;
		}
	}
#endif //__EMSCRIPTEN__

	return 0;
}
