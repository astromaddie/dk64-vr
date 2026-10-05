#ifndef DK64VR_SETTINGS_H
#define DK64VR_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#include "sfvr/sfvr_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

// <app folder>/vr.toml ($APP_FOLDER_PATH, ~/.local/share/DK64Recompiled on the Frame): a top-level `version` and a [vr] table.
// The key set is the sfvr registry plus the port keys in dk64vr_settings.c. Every
// key has a default, so a missing key is filled in, a bad value is replaced by
// the default, an out-of-range value is clamped, and keys we do not know (and
// every other line of the file) are kept as they are. The file is rewritten,
// atomically, only when something was missing or repaired. Environment
// variables DK64_VR_<KEY> override the file for the run and are never
// written back.

#define DK64VR_SETTINGS_MAX_KEYS 96

// resolution_scale = "auto": an absolute eye size (dk64vr::eyeSize), stored as this value.
#define DK64VR_RESOLUTION_AUTO 0.0

typedef struct {
	double values[DK64VR_SETTINGS_MAX_KEYS]; // effective: file value, then env override
	double file_values[DK64VR_SETTINGS_MAX_KEYS]; // what the file holds (and save() writes)
	int file_version; // schema version found in the file; 0 when absent
	char *raw; // the file text as read, NUL terminated; NULL when there was none
	size_t raw_len;
	char path[1024];
} dk64vr_settings_t;

size_t dk64vr_settings_count(void);
const sfvr_setting_key *dk64vr_settings_key(size_t index);
int dk64vr_settings_index(const char *name); // -1 when unknown
double dk64vr_settings_default(size_t index); // the registry default, or this port's

// Defaults for every key; no file, no env.
void dk64vr_settings_defaults(dk64vr_settings_t *s);

// Parse TOML text into file_values (starting from the defaults) and remember it
// as `raw`. Returns the number of repairs: keys that were missing, invalid,
// clamped or duplicated, plus 1 when the schema version is missing or old. A
// non-zero result means the file should be rewritten.
int dk64vr_settings_parse(dk64vr_settings_t *s, const char *text, size_t len);

// The file text for the current file_values: `raw` with known keys repaired in
// place and missing ones added to [vr]; everything else untouched. malloc'd.
char *dk64vr_settings_format(const dk64vr_settings_t *s, size_t *len);

// Env overrides into `values`. getenv_fn lets tests inject an environment
// (NULL means getenv). Returns how many keys were overridden.
int dk64vr_settings_apply_env(dk64vr_settings_t *s, const char *(*getenv_fn)(const char *));

// Read, repair and rewrite the file at `path` (creating it with defaults when
// missing), then apply the environment. Always leaves `s` usable; false when
// the file existed but could not be read, or a needed rewrite failed.
bool dk64vr_settings_load(dk64vr_settings_t *s, const char *path);

// Atomic write (temp file, fsync, rename) of dk64vr_settings_format().
bool dk64vr_settings_save(const dk64vr_settings_t *s);

double dk64vr_settings_get(const dk64vr_settings_t *s, const char *name); // effective value; 0 for an unknown name
void dk64vr_settings_set(dk64vr_settings_t *s, const char *name, double value); // clamps; sets effective and file value

void dk64vr_settings_free(dk64vr_settings_t *s);

#ifdef __cplusplus
}
#endif

#endif
