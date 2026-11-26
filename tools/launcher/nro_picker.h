#ifndef LEVELUP_NRO_PICKER_H
#define LEVELUP_NRO_PICKER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse a semantic version X.Y.Z (optionally prefixed with 'v'/'V') anywhere inside text.
 */
bool nro_picker_parse_semver(const char* text, int* major, int* minor, int* patch);

/**
 * Read display_version from an NRO file's embedded ASET/NACP header if present.
 */
bool nro_picker_read_nacp_version(const char* filepath, char* out_ver, size_t out_size);

/**
 * Return true if filename is a valid launch target candidate in a dedicated or shared directory.
 */
bool nro_picker_is_candidate_filename(const char* filename, bool dedicated_dir);

/**
 * Scan search_dirs in order and choose the best LevelUp NRO path.
 */
bool nro_picker_select_best(const char* const* search_dirs,
                            const bool* dedicated_flags,
                            size_t dir_count,
                            char* out_path,
                            size_t out_path_size);

/**
 * Format argv[0] for envSetNextLoad (wraps path in double quotes if it contains spaces).
 */
void nro_picker_format_argv(const char* nro_path, char* out_argv, size_t out_argv_size);

/**
 * Read the persisted NRO directory from cfg_file (first non-empty line, trimmed).
 */
bool nro_picker_read_saved_dir(const char* cfg_file, char* out_dir, size_t out_dir_size);

#ifdef __cplusplus
}
#endif

#endif // LEVELUP_NRO_PICKER_H
