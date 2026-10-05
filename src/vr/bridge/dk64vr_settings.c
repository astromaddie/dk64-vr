#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dk64vr_settings.h"
#include "dk64vr_log.h"

// Keys that only this port has. The standard keys live in the sfvr registry.
static const sfvr_setting_key port_keys[] = {
	// Game units per metre. A placeholder until the logged extent of the player model
	// ([vr] player extent ...) settles it; docs/VR_ARCHITECTURE.md section 5.
	{ "world_scale", SFVR_SETTING_FLOAT, 25.0, 5.0, 100.0, NULL },
	// Drop the game camera's pitch and roll: level in the headset is level in the game, and
	// the head supplies pitch.
	{ "level_pitch", SFVR_SETTING_BOOL, 1.0, 0.0, 1.0, NULL },
	// The flat panel for menus, transitions and anything that is not 3D play: a world-locked
	// quad over a black projection layer.
	{ "panel_distance", SFVR_SETTING_FLOAT, 2.0, 0.8, 20.0, NULL },
	{ "panel_width", SFVR_SETTING_FLOAT, 2.2, 0.5, 10.0, NULL },
	// Diagnostic: stereo whenever the frame has a camera projection, whatever the game state
	// says (the game state is UNKNOWN until the game-side patches publish it).
	{ "diag_force_stereo", SFVR_SETTING_BOOL, 0.0, 0.0, 1.0, NULL },
	// Diagnostic: for this many seconds after the game starts, press A and Start on a schedule
	// through the N64 input path, to get from the title into play without a wearer. 0 = off.
	{ "diag_autostart", SFVR_SETTING_FLOAT, 0.0, 0.0, 600.0, NULL },
	// Diagnostic: quit cleanly this many seconds after the XR session starts. 0 = off.
	{ "diag_exit_seconds", SFVR_SETTING_FLOAT, 0.0, 0.0, 3600.0, NULL },
	// dump_frame repeats: dump_count dumps in all, dump_interval frames apart, with the
	// synthetic yaw advanced by diag_yaw_step degrees after each (0, 90, 180 in one run).
	{ "dump_count", SFVR_SETTING_INT, 1.0, 1.0, 64.0, NULL },
	{ "dump_interval", SFVR_SETTING_INT, 120.0, 1.0, 100000.0, NULL },
	{ "diag_yaw_step", SFVR_SETTING_FLOAT, 0.0, -360.0, 360.0, NULL },
	// Diagnostic: dump the flat panel's image (vr-panel-*.png) at its Nth present. 0 = off.
	{ "dump_panel_frame", SFVR_SETTING_INT, 0.0, 0.0, 1000000.0, NULL }
};
#define PORT_KEY_COUNT (sizeof(port_keys) / sizeof(port_keys[0]))

#define ENV_PREFIX "DK64"

// Standard keys whose default this port sets differently from the sfvr registry: the
// display target of standard.md section 9 (120 Hz, about 2160 px an eye).
static const struct { const char *name; double def; } port_defaults[] = {
	{ "refresh_rate", 120.0 },
	{ "resolution_scale", 1.25 },
};
#define PORT_DEFAULT_COUNT (sizeof(port_defaults) / sizeof(port_defaults[0]))


size_t dk64vr_settings_count(void) {
	return sfvr_settings_count() + PORT_KEY_COUNT;
}

const sfvr_setting_key *dk64vr_settings_key(size_t index) {
	size_t standard = sfvr_settings_count();
	if (index < standard) {
		return sfvr_settings_at(index);
	}
	index -= standard;
	return index < PORT_KEY_COUNT ? &port_keys[index] : NULL;
}

int dk64vr_settings_index(const char *name) {
	if (!name) {
		return -1;
	}
	for (size_t i = 0; i < dk64vr_settings_count(); i++) {
		if (strcmp(dk64vr_settings_key(i)->name, name) == 0) {
			return (int)i;
		}
	}
	return -1;
}

double dk64vr_settings_default(size_t index) {
	const sfvr_setting_key *k = dk64vr_settings_key(index);
	if (!k) {
		return 0.0;
	}
	for (size_t i = 0; i < PORT_DEFAULT_COUNT; i++) {
		if (strcmp(port_defaults[i].name, k->name) == 0) {
			return port_defaults[i].def;
		}
	}
	return k->def;
}

void dk64vr_settings_defaults(dk64vr_settings_t *s) {
	for (size_t i = 0; i < DK64VR_SETTINGS_MAX_KEYS; i++) {
		s->values[i] = s->file_values[i] = 0;
	}
	for (size_t i = 0; i < dk64vr_settings_count(); i++) {
		s->values[i] = s->file_values[i] = dk64vr_settings_default(i);
	}
}

void dk64vr_settings_free(dk64vr_settings_t *s) {
	free(s->raw);
	s->raw = NULL;
	s->raw_len = 0;
}


// -----------------------------------------------------------------------------
// TOML subset: `version = N`, `[table]`, `key = value`, `# comment`.

typedef struct {
	const char *key;
	size_t key_len;
	const char *val;
	size_t val_len;
} kv_t;

enum { LINE_OTHER, LINE_TABLE, LINE_KEY };

static bool is_space(char c) {
	return c == ' ' || c == '\t' || c == '\r';
}

static bool is_bare_key_char(char c) {
	return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static int classify_line(const char *s, size_t len, kv_t *kv) {
	size_t i = 0;
	while (i < len && is_space(s[i])) {
		i++;
	}
	if (i >= len || s[i] == '#') {
		return LINE_OTHER;
	}

	if (s[i] == '[') {
		size_t start = ++i;
		while (i < len && s[i] != ']') {
			i++;
		}
		if (i >= len) {
			return LINE_OTHER;
		}
		size_t end = i;
		while (start < end && is_space(s[start])) {
			start++;
		}
		while (end > start && is_space(s[end - 1])) {
			end--;
		}
		kv->key = s + start;
		kv->key_len = end - start;
		return LINE_TABLE;
	}

	size_t key_start = i;
	while (i < len && is_bare_key_char(s[i])) {
		i++;
	}
	size_t key_end = i;
	while (i < len && is_space(s[i])) {
		i++;
	}
	if (key_end == key_start || i >= len || s[i] != '=') {
		return LINE_OTHER;
	}
	i++;
	while (i < len && is_space(s[i])) {
		i++;
	}

	// The value runs to an unquoted '#'
	size_t val_start = i;
	bool quoted = false;
	for (; i < len; i++) {
		if (s[i] == '\\' && quoted) {
			i++;
		}
		else if (s[i] == '"') {
			quoted = !quoted;
		}
		else if (s[i] == '#' && !quoted) {
			break;
		}
	}
	size_t val_end = i;
	while (val_end > val_start && is_space(s[val_end - 1])) {
		val_end--;
	}

	kv->key = s + key_start;
	kv->key_len = key_end - key_start;
	kv->val = s + val_start;
	kv->val_len = val_end - val_start;
	return LINE_KEY;
}

static bool same_value(double a, double b) {
	// Floats go through the registry's float clamp: 0.35 reads back as 0.3499999
	return fabs(a - b) <= 1e-6 * fmax(1.0, fabs(a));
}

// Reads `val` as a value for key `k`. Returns false when it is not valid (the
// caller uses the default). `clamped` is set when it was valid but out of range.
static bool value_parse(const sfvr_setting_key *k, const char *val, size_t len, double *out, bool *clamped) {
	char buf[64];
	*clamped = false;

	if (len >= 2 && val[0] == '"' && val[len - 1] == '"') {
		val++;
		len -= 2;
	}
	if (len == 0 || len >= sizeof(buf)) {
		return false;
	}
	memcpy(buf, val, len);
	buf[len] = '\0';

	if (k->type == SFVR_SETTING_BOOL) {
		int b = sfvr_settings_parse_bool(buf, -1);
		if (b < 0) {
			return false;
		}
		*out = b;
		return true;
	}

	if (k->type == SFVR_SETTING_ENUM) {
		int index = sfvr_settings_parse_enum(k, buf);
		const char *name = k->enum_values[index];
		const char *a = buf;
		while (is_space(*a)) {
			a++;
		}
		size_t n = strlen(name);
		if (strlen(a) < n) {
			return false;
		}
		for (size_t i = 0; i < n; i++) {
			if (tolower((unsigned char)a[i]) != name[i]) {
				return false;
			}
		}
		for (const char *rest = a + n; *rest; rest++) {
			if (!is_space(*rest)) {
				return false;
			}
		}
		*out = index;
		return true;
	}

	char *end = NULL;
	double v = strtod(buf, &end);
	if (end == buf || isnan(v)) {
		return false;
	}
	while (*end && is_space(*end)) {
		end++;
	}
	if (*end) {
		return false;
	}

	double result;
	if (k->type == SFVR_SETTING_INT) {
		double rounded = floor(v + 0.5);
		if (rounded > 2147483647.0 || rounded < -2147483648.0) {
			rounded = rounded > 0 ? 2147483647.0 : -2147483648.0;
		}
		result = sfvr_settings_clamp_int(k, (int)rounded);
	}
	else {
		result = sfvr_settings_clamp_float(k, (float)v);
	}
	*clamped = !same_value(result, v);
	*out = result;
	return true;
}

static int value_render(const sfvr_setting_key *k, double v, char *out, size_t n) {
	switch (k->type) {
		case SFVR_SETTING_BOOL:
			return snprintf(out, n, "%s", v != 0 ? "true" : "false");
		case SFVR_SETTING_INT:
			return snprintf(out, n, "%d", (int)v);
		case SFVR_SETTING_ENUM: {
			int count = sfvr_settings_enum_count(k);
			int index = (int)v;
			if (index < 0 || index >= count) {
				index = (int)k->def;
			}
			return snprintf(out, n, "\"%s\"", k->enum_values[index]);
		}
		default: {
			int len = snprintf(out, n, "%.6g", v);
			if (len > 0 && (size_t)len + 2 < n && !strpbrk(out, ".en")) {
				strcat(out, ".0"); // TOML floats need a dot
				len += 2;
			}
			return len;
		}
	}
}


// -----------------------------------------------------------------------------
// Parse

static bool name_is(const kv_t *kv, const char *name) {
	return kv->key_len == strlen(name) && memcmp(kv->key, name, kv->key_len) == 0;
}

static bool key_name_index(const kv_t *kv, int *index) {
	char name[64];
	if (kv->key_len >= sizeof(name)) {
		return false;
	}
	memcpy(name, kv->key, kv->key_len);
	name[kv->key_len] = '\0';
	*index = dk64vr_settings_index(name);
	return *index >= 0;
}

// Calls fn for each line. `table` is the current table name ("" at the top).
typedef void (*line_fn)(void *user, const char *line, size_t len, int kind, const kv_t *kv, const char *table);

static void each_line(const char *text, size_t len, line_fn fn, void *user) {
	char table[64] = "";
	size_t pos = 0;
	while (pos < len) {
		const char *line = text + pos;
		const char *nl = memchr(line, '\n', len - pos);
		size_t line_len = nl ? (size_t)(nl - line) : len - pos;
		pos += line_len + (nl ? 1 : 0);

		kv_t kv = {0};
		int kind = classify_line(line, line_len, &kv);
		if (kind == LINE_TABLE) {
			size_t n = kv.key_len < sizeof(table) - 1 ? kv.key_len : sizeof(table) - 1;
			memcpy(table, kv.key, n);
			table[n] = '\0';
		}
		fn(user, line, line_len, kind, &kv, table);
	}
}

typedef struct {
	dk64vr_settings_t *s;
	bool seen[DK64VR_SETTINGS_MAX_KEYS];
	int repairs;
} parse_state_t;

static void parse_line(void *user, const char *line, size_t len, int kind, const kv_t *kv, const char *table) {
	(void)line; (void)len;
	parse_state_t *p = user;
	if (kind != LINE_KEY) {
		return;
	}

	if (table[0] == '\0' && name_is(kv, "version")) {
		char number[16];
		size_t n = kv->val_len < sizeof(number) - 1 ? kv->val_len : sizeof(number) - 1;
		memcpy(number, kv->val, n);
		number[n] = '\0';
		p->s->file_version = atoi(number);
		return;
	}

	int index;
	if (strcmp(table, "vr") != 0 || !key_name_index(kv, &index)) {
		return; // not ours: kept as is
	}
	if (p->seen[index]) {
		p->repairs++; // a duplicate; the first one wins
		return;
	}
	p->seen[index] = true;

	double v;
	bool clamped;
	if (!value_parse(dk64vr_settings_key(index), kv->val, kv->val_len, &v, &clamped)) {
		p->repairs++; // the default stays
	}
	else {
		p->s->file_values[index] = v;
		p->repairs += clamped;
	}
}

int dk64vr_settings_parse(dk64vr_settings_t *s, const char *text, size_t len) {
	for (size_t i = 0; i < dk64vr_settings_count(); i++) {
		s->file_values[i] = dk64vr_settings_default(i);
	}
	s->file_version = 0;

	free(s->raw);
	s->raw = malloc(len + 1);
	if (s->raw) {
		memcpy(s->raw, text, len);
		s->raw[len] = '\0';
		s->raw_len = len;
	}
	else {
		s->raw_len = 0;
	}

	parse_state_t p = { .s = s };
	each_line(text, len, parse_line, &p);

	for (size_t i = 0; i < dk64vr_settings_count(); i++) {
		if (!p.seen[i]) {
			p.repairs++;
		}
	}
	if (s->file_version < SFVR_SETTINGS_SCHEMA) {
		p.repairs++;
	}
	return p.repairs;
}


// -----------------------------------------------------------------------------
// Format

typedef struct {
	char **lines;
	size_t len;
	size_t cap;
} lines_t;

static bool lines_add_at(lines_t *l, size_t at, const char *text, size_t n) {
	if (l->len == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 64;
		char **lines = realloc(l->lines, cap * sizeof(char *));
		if (!lines) {
			return false;
		}
		l->lines = lines;
		l->cap = cap;
	}
	char *copy = malloc(n + 1);
	if (!copy) {
		return false;
	}
	memcpy(copy, text, n);
	copy[n] = '\0';
	memmove(&l->lines[at + 1], &l->lines[at], (l->len - at) * sizeof(char *));
	l->lines[at] = copy;
	l->len++;
	return true;
}

static void lines_free(lines_t *l) {
	for (size_t i = 0; i < l->len; i++) {
		free(l->lines[i]);
	}
	free(l->lines);
}

typedef struct {
	const dk64vr_settings_t *s;
	lines_t out;
	bool seen[DK64VR_SETTINGS_MAX_KEYS];
	bool have_version;
	bool have_vr_table;
	size_t vr_insert_at; // index in out after the last key (or the header) of [vr]
	char failed;
} format_state_t;

static void format_line(void *user, const char *line, size_t len, int kind, const kv_t *kv, const char *table) {
	format_state_t *f = user;
	const dk64vr_settings_t *s = f->s;
	char text[160];

	if (kind == LINE_TABLE && strcmp(table, "vr") == 0 && !f->have_vr_table) {
		f->have_vr_table = true;
		f->vr_insert_at = f->out.len + 1;
	}

	if (kind == LINE_KEY && table[0] == '\0' && name_is(kv, "version")) {
		f->have_version = true;
		if (s->file_version < SFVR_SETTINGS_SCHEMA) {
			snprintf(text, sizeof(text), "version = %d", SFVR_SETTINGS_SCHEMA);
			f->failed |= !lines_add_at(&f->out, f->out.len, text, strlen(text));
			return;
		}
	}
	else if (kind == LINE_KEY && strcmp(table, "vr") == 0) {
		int index;
		if (key_name_index(kv, &index)) {
			if (f->seen[index]) {
				return; // drop a duplicate
			}
			f->seen[index] = true;

			double v;
			bool clamped;
			const sfvr_setting_key *k = dk64vr_settings_key(index);
			bool keep = value_parse(k, kv->val, kv->val_len, &v, &clamped) && !clamped && same_value(v, s->file_values[index]);
			if (!keep) {
				int n = snprintf(text, sizeof(text), "%s = ", k->name);
				value_render(k, s->file_values[index], text + n, sizeof(text) - n);
				f->failed |= !lines_add_at(&f->out, f->out.len, text, strlen(text));
				f->vr_insert_at = f->out.len;
				return;
			}
		}
	}

	f->failed |= !lines_add_at(&f->out, f->out.len, line, len);
	if (kind == LINE_KEY && strcmp(table, "vr") == 0) {
		f->vr_insert_at = f->out.len;
	}
}

char *dk64vr_settings_format(const dk64vr_settings_t *s, size_t *out_len) {
	format_state_t f = { .s = s };
	const char *raw = s->raw ? s->raw : "";
	size_t raw_len = s->raw ? s->raw_len : 0;

	if (raw_len == 0) {
		const char *header = "# DK64 VR settings. DK64_VR_<KEY> environment variables override them.";
		f.failed |= !lines_add_at(&f.out, 0, header, strlen(header));
	}
	each_line(raw, raw_len, format_line, &f);

	if (!f.have_version) {
		char text[32];
		snprintf(text, sizeof(text), "version = %d", SFVR_SETTINGS_SCHEMA);
		size_t at = 0;
		// after a leading comment block, so a header comment stays on top
		while (at < f.out.len && f.out.lines[at][0] == '#') {
			at++;
		}
		f.failed |= !lines_add_at(&f.out, at, text, strlen(text));
		if (f.have_vr_table) {
			f.vr_insert_at++;
		}
	}

	if (!f.have_vr_table) {
		if (f.out.len > 0 && f.out.lines[f.out.len - 1][0] != '\0') {
			f.failed |= !lines_add_at(&f.out, f.out.len, "", 0);
		}
		f.failed |= !lines_add_at(&f.out, f.out.len, "[vr]", 4);
		f.vr_insert_at = f.out.len;
	}

	for (size_t i = 0; i < dk64vr_settings_count(); i++) {
		if (f.seen[i]) {
			continue;
		}
		char text[160];
		const sfvr_setting_key *k = dk64vr_settings_key(i);
		int n = snprintf(text, sizeof(text), "%s = ", k->name);
		value_render(k, s->file_values[i], text + n, sizeof(text) - n);
		f.failed |= !lines_add_at(&f.out, f.vr_insert_at++, text, strlen(text));
	}

	size_t total = 0;
	for (size_t i = 0; i < f.out.len; i++) {
		total += strlen(f.out.lines[i]) + 1;
	}
	char *result = f.failed ? NULL : malloc(total + 1);
	if (result) {
		char *p = result;
		for (size_t i = 0; i < f.out.len; i++) {
			size_t n = strlen(f.out.lines[i]);
			memcpy(p, f.out.lines[i], n);
			p += n;
			*p++ = '\n';
		}
		*p = '\0';
		if (out_len) {
			*out_len = total;
		}
	}
	lines_free(&f.out);
	return result;
}


// -----------------------------------------------------------------------------
// Environment, files

static const char *default_getenv(const char *name) {
	return getenv(name);
}

int dk64vr_settings_apply_env(dk64vr_settings_t *s, const char *(*getenv_fn)(const char *)) {
	if (!getenv_fn) {
		getenv_fn = default_getenv;
	}

	int applied = 0;
	for (size_t i = 0; i < dk64vr_settings_count(); i++) {
		const sfvr_setting_key *k = dk64vr_settings_key(i);
		char name[96];
		if (!sfvr_settings_env_name(ENV_PREFIX, k->name, name, sizeof(name))) {
			continue;
		}
		const char *text = getenv_fn(name);
		if (!text || !text[0]) {
			continue;
		}

		double v;
		bool clamped;
		if (value_parse(k, text, strlen(text), &v, &clamped)) {
			s->values[i] = v;
			applied++;
			dk64vr_logf("settings: %s=%s from the environment", name, text);
		}
		else {
			dk64vr_logf("settings: ignoring %s=%s (not a valid value)", name, text);
		}
	}
	return applied;
}

static char *read_file(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	size_t cap = 4096, used = 0;
	char *buf = malloc(cap);
	while (buf) {
		size_t n = fread(buf + used, 1, cap - used, f);
		used += n;
		if (used < cap) {
			break;
		}
		cap *= 2;
		char *bigger = realloc(buf, cap);
		if (!bigger) {
			free(buf);
			buf = NULL;
		}
		else {
			buf = bigger;
		}
	}
	fclose(f);
	*len = used;
	return buf;
}

bool dk64vr_settings_save(const dk64vr_settings_t *s) {
	size_t len = 0;
	char *text = dk64vr_settings_format(s, &len);
	if (!text) {
		return false;
	}

	char tmp[sizeof(s->path) + 8];
	snprintf(tmp, sizeof(tmp), "%s.tmp", s->path);

	bool ok = false;
	FILE *f = fopen(tmp, "wb");
	if (f) {
		ok = fwrite(text, 1, len, f) == len;
		ok = (fflush(f) == 0) && ok;
		ok = (fsync(fileno(f)) == 0) && ok;
		ok = (fclose(f) == 0) && ok;
		ok = ok && rename(tmp, s->path) == 0;
		if (!ok) {
			remove(tmp);
		}
	}
	free(text);
	return ok;
}

bool dk64vr_settings_load(dk64vr_settings_t *s, const char *path) {
	memset(s, 0, sizeof(*s));
	snprintf(s->path, sizeof(s->path), "%s", path);
	dk64vr_settings_defaults(s);

	size_t len = 0;
	char *text = read_file(path, &len);
	bool ok = true;
	int repairs;
	if (text) {
		repairs = dk64vr_settings_parse(s, text, len);
		free(text);
	}
	else {
		repairs = dk64vr_settings_parse(s, "", 0);
	}

	for (size_t i = 0; i < dk64vr_settings_count(); i++) {
		s->values[i] = s->file_values[i];
	}

	if (repairs > 0) {
		if (dk64vr_settings_save(s)) {
			dk64vr_logf("settings: wrote %s (%d added or repaired)", path, repairs);
		}
		else {
			dk64vr_logf("settings: could not write %s; using the values in memory", path);
			ok = false;
		}
	}
	dk64vr_settings_apply_env(s, NULL);
	return ok;
}

double dk64vr_settings_get(const dk64vr_settings_t *s, const char *name) {
	int index = dk64vr_settings_index(name);
	return index >= 0 ? s->values[index] : 0.0;
}

void dk64vr_settings_set(dk64vr_settings_t *s, const char *name, double value) {
	int index = dk64vr_settings_index(name);
	if (index < 0) {
		return;
	}
	const sfvr_setting_key *k = dk64vr_settings_key(index);
	if (k->type == SFVR_SETTING_INT || k->type == SFVR_SETTING_ENUM) {
		value = sfvr_settings_clamp_int(k, (int)floor(value + 0.5));
	}
	else if (k->type == SFVR_SETTING_BOOL) {
		value = value != 0;
	}
	else {
		value = sfvr_settings_clamp_float(k, (float)value);
	}
	s->values[index] = s->file_values[index] = value;
}
