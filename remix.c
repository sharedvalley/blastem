#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "remix.h"
#define JSMN_STATIC
#define JSMN_STRICT
#include "jsmn.h"

#define REMIX_FORMAT 1
#define MAX_PATCHES 256
#define MAX_CONDITIONS 64
#define FOREVER UINT32_MAX

enum {
	OP_EQ,
	OP_NE,
	OP_LT,
	OP_LE,
	OP_GT,
	OP_GE
};

//a place in one of the core's regions
typedef struct {
	uint32_t offset;
	uint8_t  region;
	uint8_t  size;
} location;

typedef struct {
	location where;
	uint32_t value;
	uint32_t frame;
	uint32_t until;
	uint8_t  hold;
} patch;

typedef struct {
	location where;
	uint32_t value;
	uint8_t  op;
} condition;

typedef struct {
	char      *system;
	patch     *patches;
	condition *win;
	condition *lose;
	uint32_t  num_patches;
	uint32_t  num_win;
	uint32_t  num_lose;
	uint32_t  time_limit;
} remix;

static remix *current;
static const emu_api *running;
static uint32_t frame, outcome_frame;
static uint8_t outcome;
static remix_outcome_handler outcome_handler;
static char why[256];

//the JSON being parsed
static const char *text;
static jsmntok_t *tokens;
static int num_tokens;

static uint8_t fail(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(why, sizeof(why), format, args);
	va_end(args);
	return 0;
}

static void free_remix(remix *r)
{
	if (r) {
		free(r->system);
		free(r->patches);
		free(r->win);
		free(r->lose);
		free(r);
	}
}

//the token after the whole of the value at index, by position, as a value nested in
//the JSON deeply enough would overflow the stack if this recursed
static int after(int index)
{
	int end = tokens[index].end;
	for (index++; index < num_tokens && tokens[index].start < end; index++)
	{
	}
	return index;
}

static uint8_t token_is(int index, const char *value)
{
	size_t length = tokens[index].end - tokens[index].start;
	return tokens[index].type == JSMN_STRING && length == strlen(value) && !memcmp(text + tokens[index].start, value, length);
}

//the value of key in the object at index, -1 if it has none
static int field(int index, const char *key)
{
	int cur = index + 1;
	for (int i = 0; i < tokens[index].size; i++)
	{
		if (token_is(cur, key)) {
			return cur + 1;
		}
		cur = after(cur + 1);
	}
	return -1;
}

//a decimal number, or a hexadecimal one starting with 0x, as a JSON number or string
static uint8_t token_number(int index, uint32_t *out)
{
	if (tokens[index].type != JSMN_PRIMITIVE && tokens[index].type != JSMN_STRING) {
		return 0;
	}
	const char *cur = text + tokens[index].start, *end = text + tokens[index].end;
	uint32_t base = 10;
	if (end - cur > 2 && cur[0] == '0' && (cur[1] == 'x' || cur[1] == 'X')) {
		base = 16;
		cur += 2;
	}
	if (cur == end) {
		return 0;
	}
	uint64_t value = 0;
	for (; cur < end; cur++)
	{
		uint32_t digit;
		if (*cur >= '0' && *cur <= '9') {
			digit = *cur - '0';
		} else if (base == 16 && *cur >= 'a' && *cur <= 'f') {
			digit = *cur - 'a' + 10;
		} else if (base == 16 && *cur >= 'A' && *cur <= 'F') {
			digit = *cur - 'A' + 10;
		} else {
			return 0;
		}
		value = value * base + digit;
		if (value > UINT32_MAX) {
			return 0;
		}
	}
	*out = value;
	return 1;
}

//the number in key of the object at index, or fallback if it has none
static uint8_t number_field(int index, const char *key, uint32_t fallback, uint32_t *out, const char *where)
{
	int value = field(index, key);
	if (value < 0) {
		*out = fallback;
		return 1;
	}
	if (!token_number(value, out)) {
		return fail("%s: %s isn't a whole number from 0 to 4294967295", where, key);
	}
	return 1;
}

static uint8_t parse_location(int index, const emu_api *api, location *out, const char *where)
{
	if (tokens[index].type != JSMN_OBJECT) {
		return fail("%s isn't an object", where);
	}
	int name = field(index, "region");
	if (name < 0) {
		return fail("%s has no region", where);
	}
	const emu_region *region = NULL;
	for (uint32_t i = 0; i < api->num_regions && !region; i++)
	{
		if (token_is(name, api->regions[i].name)) {
			region = api->regions + i;
			out->region = i;
		}
	}
	if (!region) {
		return fail("%s: %.*s isn't a region of this system", where, tokens[name].end - tokens[name].start, text + tokens[name].start);
	}
	int address_value = field(index, "address");
	uint32_t address, size;
	if (address_value < 0 || !token_number(address_value, &address)) {
		return fail("%s needs an address", where);
	}
	if (!number_field(index, "size", 1, &size, where)) {
		return 0;
	}
	if (size != 1 && size != 2 && size != 4) {
		return fail("%s: size must be 1, 2 or 4", where);
	}
	if (address < region->base || address - region->base > region->size - size) {
		return fail("%s: address $%X isn't in %s, $%X to $%X", where, address, region->name, region->base, region->base + region->size - 1);
	}
	out->offset = address - region->base;
	out->size = size;
	return 1;
}

static uint8_t parse_value(int index, uint8_t size, uint32_t *out, const char *where)
{
	int value = field(index, "value");
	if (value < 0 || !token_number(value, out)) {
		return fail("%s needs a value", where);
	}
	if (size < 4 && *out >> (size * 8)) {
		return fail("%s: value $%X doesn't fit in %d byte%s", where, *out, size, size > 1 ? "s" : "");
	}
	return 1;
}

static uint8_t parse_patches(int index, const emu_api *api, remix *out)
{
	if (tokens[index].type != JSMN_ARRAY) {
		return fail("patches isn't a list");
	}
	if (tokens[index].size > MAX_PATCHES) {
		return fail("A remix can have %d patches at most", MAX_PATCHES);
	}
	out->patches = calloc(tokens[index].size + 1, sizeof(patch));
	int cur = index + 1;
	for (int i = 0; i < tokens[index].size; i++, cur = after(cur))
	{
		char where[32];
		snprintf(where, sizeof(where), "patches[%d]", i);
		patch *p = out->patches + i;
		if (!parse_location(cur, api, &p->where, where) || !parse_value(cur, p->where.size, &p->value, where)
			|| !number_field(cur, "frame", 0, &p->frame, where) || !number_field(cur, "until", FOREVER, &p->until, where)) {
			return 0;
		}
		int mode = field(cur, "mode");
		if (mode >= 0 && !token_is(mode, "once") && !token_is(mode, "hold")) {
			return fail("%s: mode must be once or hold", where);
		}
		p->hold = mode >= 0 && token_is(mode, "hold");
		if (p->hold && p->until <= p->frame) {
			return fail("%s: until must be after frame", where);
		}
		out->num_patches++;
	}
	return 1;
}

static uint8_t parse_op(int index, uint8_t *out)
{
	static const char *ops[] = {"==", "!=", "<", "<=", ">", ">="};
	for (uint8_t i = 0; i < sizeof(ops) / sizeof(*ops); i++)
	{
		if (token_is(index, ops[i])) {
			*out = i;
			return 1;
		}
	}
	return 0;
}

static uint8_t parse_conditions(int index, const emu_api *api, const char *name, condition **out, uint32_t *count)
{
	if (tokens[index].type != JSMN_ARRAY) {
		return fail("objective.%s isn't a list", name);
	}
	if (tokens[index].size > MAX_CONDITIONS) {
		return fail("objective.%s can have %d conditions at most", name, MAX_CONDITIONS);
	}
	*out = calloc(tokens[index].size + 1, sizeof(condition));
	int cur = index + 1;
	for (int i = 0; i < tokens[index].size; i++, cur = after(cur))
	{
		char where[40];
		snprintf(where, sizeof(where), "objective.%s[%d]", name, i);
		condition *c = *out + i;
		if (!parse_location(cur, api, &c->where, where) || !parse_value(cur, c->where.size, &c->value, where)) {
			return 0;
		}
		int op = field(cur, "op");
		if (op < 0 || !parse_op(op, &c->op)) {
			return fail("%s: op must be ==, !=, <, <=, > or >=", where);
		}
		++*count;
	}
	return 1;
}

static uint8_t parse_objective(int index, const emu_api *api, remix *out)
{
	if (tokens[index].type != JSMN_OBJECT) {
		return fail("objective isn't an object");
	}
	if (!number_field(index, "timeLimit", 0, &out->time_limit, "objective")) {
		return 0;
	}
	int win = field(index, "win"), lose = field(index, "lose");
	return (win < 0 || parse_conditions(win, api, "win", &out->win, &out->num_win))
		&& (lose < 0 || parse_conditions(lose, api, "lose", &out->lose, &out->num_lose));
}

static uint8_t parse_root(const emu_api *api, remix *out)
{
	if (tokens[0].type != JSMN_OBJECT || after(0) != num_tokens) {
		return fail("A remix has to be one JSON object");
	}
	int format = field(0, "format");
	uint32_t format_number;
	if (format < 0 || !token_number(format, &format_number)) {
		return fail("A remix needs a format");
	}
	if (format_number != REMIX_FORMAT) {
		return fail("This remix has format %u, and this version of BlastEm reads format %d", format_number, REMIX_FORMAT);
	}
	int system = field(0, "system");
	if (system < 0 || !token_is(system, api->system)) {
		return fail("This remix isn't for this system, %s", api->system);
	}
	out->system = strdup(api->system);
	int patches = field(0, "patches"), objective = field(0, "objective");
	return (patches < 0 || parse_patches(patches, api, out)) && (objective < 0 || parse_objective(objective, api, out));
}

uint8_t remix_parse(const char *json, size_t size, const emu_api *api)
{
	if (!api) {
		return fail("This system can't be remixed");
	}
	if (size > REMIX_MAX_SIZE) {
		return fail("A remix can be %d bytes at most", REMIX_MAX_SIZE);
	}
	jsmn_parser parser;
	jsmn_init(&parser);
	num_tokens = jsmn_parse(&parser, json, size, NULL, 0);
	if (num_tokens <= 0) {
		return fail("This remix isn't valid JSON");
	}
	tokens = malloc(num_tokens * sizeof(jsmntok_t));
	text = json;
	jsmn_init(&parser);
	remix *parsed = calloc(1, sizeof(remix));
	uint8_t ok = jsmn_parse(&parser, json, size, tokens, num_tokens) == num_tokens ? parse_root(api, parsed) : fail("This remix isn't valid JSON");
	free(tokens);
	tokens = NULL;
	if (!ok) {
		free_remix(parsed);
		return 0;
	}
	remix_discard();
	current = parsed;
	return 1;
}

void remix_discard(void)
{
	remix_stop();
	free_remix(current);
	current = NULL;
}

const char *remix_why(void)
{
	return why;
}

void remix_start(const emu_api *api)
{
	running = current && api && !strcmp(api->system, current->system) ? api : NULL;
	frame = outcome_frame = 0;
	outcome = REMIX_NONE;
}

void remix_stop(void)
{
	running = NULL;
}

static uint32_t read_value(const emu_region *region, uint32_t offset, uint8_t size)
{
	uint32_t value = 0;
	for (uint8_t i = 0; i < size; i++)
	{
		uint32_t byte = region->read8(region->data, offset + i);
		value |= byte << (region->big_endian ? (size - 1 - i) * 8 : i * 8);
	}
	return value;
}

static void write_location(location where, uint32_t value)
{
	const emu_region *region = running->regions + where.region;
	for (uint8_t i = 0; i < where.size; i++)
	{
		region->write8(region->data, where.offset + i, value >> (region->big_endian ? (where.size - 1 - i) * 8 : i * 8));
	}
}

static uint8_t condition_holds(const condition *c)
{
	uint32_t value = read_value(running->regions + c->where.region, c->where.offset, c->where.size);
	switch (c->op)
	{
	case OP_EQ: return value == c->value;
	case OP_NE: return value != c->value;
	case OP_LT: return value < c->value;
	case OP_LE: return value <= c->value;
	case OP_GT: return value > c->value;
	default:    return value >= c->value;
	}
}

//an empty list never holds, or a remix without conditions would end on its first frame
static uint8_t all_hold(const condition *conditions, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++)
	{
		if (!condition_holds(conditions + i)) {
			return 0;
		}
	}
	return count != 0;
}

static void end_objective(uint8_t result)
{
	outcome = result;
	outcome_frame = frame;
	if (outcome_handler) {
		outcome_handler(outcome, outcome_frame);
	}
}

void remix_frame(void)
{
	if (!running) {
		return;
	}
	for (uint32_t i = 0; i < current->num_patches; i++)
	{
		const patch *p = current->patches + i;
		if (p->hold ? frame >= p->frame && frame < p->until : frame == p->frame) {
			write_location(p->where, p->value);
		}
	}
	if (outcome == REMIX_NONE) {
		if (all_hold(current->win, current->num_win)) {
			end_objective(REMIX_WIN);
		} else if (all_hold(current->lose, current->num_lose) || (current->time_limit && frame >= current->time_limit)) {
			end_objective(REMIX_LOSE);
		}
	}
	frame++;
}

uint8_t remix_outcome(void)
{
	return outcome;
}

uint32_t remix_outcome_frame(void)
{
	return outcome == REMIX_NONE ? frame : outcome_frame;
}

void remix_on_outcome(remix_outcome_handler handler)
{
	outcome_handler = handler;
}

uint8_t remix_peek(const emu_api *api, const char *name, uint32_t address, uint8_t size, uint32_t *out)
{
	if (!api || (size != 1 && size != 2 && size != 4)) {
		return 0;
	}
	for (uint32_t i = 0; i < api->num_regions; i++)
	{
		const emu_region *region = api->regions + i;
		if (strcmp(region->name, name)) {
			continue;
		}
		if (address < region->base || address - region->base > region->size - size) {
			return 0;
		}
		*out = read_value(region, address - region->base, size);
		return 1;
	}
	return 0;
}
