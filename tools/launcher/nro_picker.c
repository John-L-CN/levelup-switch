#include "nro_picker.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static char ascii_tolower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static bool equals_ci(const char* a, const char* b) {
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (ascii_tolower(*a) != ascii_tolower(*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static bool starts_with_ci(const char* s, const char* prefix) {
    if (!s || !prefix) {
        return false;
    }
    while (*prefix) {
        if (ascii_tolower(*s) != ascii_tolower(*prefix)) {
            return false;
        }
        ++s;
        ++prefix;
    }
    return true;
}

static bool ends_with_ci(const char* s, const char* suffix) {
    if (!s || !suffix) {
        return false;
    }
    const size_t slen = strlen(s);
    const size_t suflen = strlen(suffix);
    if (slen < suflen) {
        return false;
    }
    return equals_ci(s + (slen - suflen), suffix);
}

static uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= ((uint64_t)p[i]) << (i * 8);
    }
    return v;
}

static bool parse_uint_part(const char** p, int* out_val) {
    const char* cur = *p;
    if (!isdigit((unsigned char)*cur)) {
        return false;
    }
    long long val = 0;
    while (isdigit((unsigned char)*cur)) {
        val = val * 10 + (*cur - '0');
        if (val > INT_MAX / 2) {
            val = INT_MAX / 2;
        }
        ++cur;
    }
    *out_val = (int)val;
    *p = cur;
    return true;
}

bool nro_picker_parse_semver(const char* text, int* major, int* minor, int* patch) {
    if (!text || !major || !minor || !patch) {
        return false;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (!isdigit((unsigned char)text[i])) {
            continue;
        }
        if (i > 0 && isdigit((unsigned char)text[i - 1])) {
            continue;
        }
        if (i >= 2 && text[i - 1] == '.' && isdigit((unsigned char)text[i - 2])) {
            continue;
        }
        const char* p = text + i;
        int maj = 0;
        int min = 0;
        int pat = 0;
        if (!parse_uint_part(&p, &maj) || *p != '.') {
            continue;
        }
        ++p;
        if (!parse_uint_part(&p, &min) || *p != '.') {
            continue;
        }
        ++p;
        if (!parse_uint_part(&p, &pat)) {
            continue;
        }
        // Reject 4-segment numeric versions like 1.2.3.4 while allowing ".nro".
        if (*p == '.' && isdigit((unsigned char)p[1])) {
            continue;
        }
        *major = maj;
        *minor = min;
        *patch = pat;
        return true;
    }
    return false;
}

bool nro_picker_read_nacp_version(const char* filepath, char* out_ver, size_t out_size) {
    if (!filepath || !out_ver || out_size == 0) {
        return false;
    }
    out_ver[0] = '\0';

    FILE* f = fopen(filepath, "rb");
    if (!f) {
        return false;
    }

    uint8_t nro_hdr[0x20];
    if (fread(nro_hdr, 1, sizeof(nro_hdr), f) != sizeof(nro_hdr)) {
        fclose(f);
        return false;
    }
    if (memcmp(nro_hdr + 0x10, "NRO0", 4) != 0) {
        fclose(f);
        return false;
    }
    const uint32_t nro_size = read_u32_le(nro_hdr + 0x18);
    if (nro_size < 0x80) {
        fclose(f);
        return false;
    }

    if (fseek(f, (long)nro_size, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    uint8_t aset_hdr[0x28];
    if (fread(aset_hdr, 1, sizeof(aset_hdr), f) != sizeof(aset_hdr)) {
        fclose(f);
        return false;
    }
    if (memcmp(aset_hdr, "ASET", 4) != 0) {
        fclose(f);
        return false;
    }
    const uint64_t nacp_offset = read_u64_le(aset_hdr + 0x18);
    const uint64_t nacp_size = read_u64_le(aset_hdr + 0x20);
    // NacpStruct.display_version sits at offset 0x3060 (16 bytes).
    if (nacp_size < 0x3070 || nacp_offset > (uint64_t)LONG_MAX - nro_size - 0x3070) {
        fclose(f);
        return false;
    }

    const long ver_pos = (long)((uint64_t)nro_size + nacp_offset + 0x3060ULL);
    if (fseek(f, ver_pos, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    char raw_ver[16];
    if (fread(raw_ver, 1, sizeof(raw_ver), f) != sizeof(raw_ver)) {
        fclose(f);
        return false;
    }
    fclose(f);

    size_t len = 0;
    while (len < sizeof(raw_ver) && raw_ver[len] != '\0') {
        ++len;
    }
    while (len > 0 && isspace((unsigned char)raw_ver[len - 1])) {
        --len;
    }
    if (len == 0) {
        return false;
    }
    const size_t copy_len = len < out_size - 1 ? len : out_size - 1;
    memcpy(out_ver, raw_ver, copy_len);
    out_ver[copy_len] = '\0';
    return true;
}

bool nro_picker_is_candidate_filename(const char* filename, bool dedicated_dir) {
    if (!filename || filename[0] == '\0') {
        return false;
    }
    if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
        return false;
    }
    // Hidden/AppleDouble metadata files should never be picked.
    if (filename[0] == '.') {
        return false;
    }
    if (!ends_with_ci(filename, ".nro")) {
        return false;
    }
    // Never chain-load the launcher itself.
    if (equals_ci(filename, "launcher.nro") || equals_ci(filename, "levelup-launcher.nro")) {
        return false;
    }
    if (!dedicated_dir && !starts_with_ci(filename, "levelup")) {
        return false;
    }
    return true;
}

typedef struct {
    bool valid;
    bool is_levelup_name;
    bool has_semver;
    int major;
    int minor;
    int patch;
    time_t mtime;
    char filename[256];
    char full_path[1024];
} PickerCandidate;

static bool candidate_is_better(const PickerCandidate* cand, const PickerCandidate* best) {
    if (!best->valid) {
        return true;
    }
    if (cand->is_levelup_name != best->is_levelup_name) {
        return cand->is_levelup_name;
    }
    if (cand->has_semver != best->has_semver) {
        return cand->has_semver;
    }
    if (cand->has_semver) {
        if (cand->major != best->major) return cand->major > best->major;
        if (cand->minor != best->minor) return cand->minor > best->minor;
        if (cand->patch != best->patch) return cand->patch > best->patch;
    }
    if (cand->mtime != best->mtime) {
        return cand->mtime > best->mtime;
    }
    return strcmp(cand->filename, best->filename) > 0;
}

bool nro_picker_select_best(const char* const* search_dirs,
                            const bool* dedicated_flags,
                            size_t dir_count,
                            char* out_path,
                            size_t out_path_size) {
    if (!search_dirs || !dedicated_flags || dir_count == 0 || !out_path || out_path_size == 0) {
        return false;
    }
    out_path[0] = '\0';

    PickerCandidate best;
    memset(&best, 0, sizeof(best));

    for (size_t d = 0; d < dir_count; ++d) {
        const char* dir_path = search_dirs[d];
        if (!dir_path || dir_path[0] == '\0') {
            continue;
        }
        DIR* dir = opendir(dir_path);
        if (!dir) {
            continue;
        }
        const size_t dir_len = strlen(dir_path);
        const bool has_slash = dir_len > 0 && dir_path[dir_len - 1] == '/';

        struct dirent* ent = NULL;
        while ((ent = readdir(dir)) != NULL) {
            if (!nro_picker_is_candidate_filename(ent->d_name, dedicated_flags[d])) {
                continue;
            }
            PickerCandidate cand;
            memset(&cand, 0, sizeof(cand));
            int n = snprintf(cand.full_path,
                             sizeof(cand.full_path),
                             has_slash ? "%s%s" : "%s/%s",
                             dir_path,
                             ent->d_name);
            if (n <= 0 || (size_t)n >= sizeof(cand.full_path)) {
                continue;
            }
            struct stat st;
            if (stat(cand.full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            cand.valid = true;
            cand.is_levelup_name = starts_with_ci(ent->d_name, "levelup");
            cand.mtime = st.st_mtime;
            snprintf(cand.filename, sizeof(cand.filename), "%s", ent->d_name);

            char nacp_ver[24];
            if (nro_picker_read_nacp_version(cand.full_path, nacp_ver, sizeof(nacp_ver)) &&
                nro_picker_parse_semver(nacp_ver, &cand.major, &cand.minor, &cand.patch)) {
                cand.has_semver = true;
            } else if (nro_picker_parse_semver(ent->d_name, &cand.major, &cand.minor, &cand.patch)) {
                cand.has_semver = true;
            }

            if (candidate_is_better(&cand, &best)) {
                best = cand;
            }
        }
        closedir(dir);
    }

    if (!best.valid) {
        return false;
    }
    const int written = snprintf(out_path, out_path_size, "%s", best.full_path);
    return written > 0 && (size_t)written < out_path_size;
}

void nro_picker_format_argv(const char* nro_path, char* out_argv, size_t out_argv_size) {
    if (!out_argv || out_argv_size == 0) {
        return;
    }
    out_argv[0] = '\0';
    if (!nro_path || nro_path[0] == '\0') {
        return;
    }
    if (strchr(nro_path, ' ') != NULL) {
        snprintf(out_argv, out_argv_size, "\"%s\"", nro_path);
    } else {
        snprintf(out_argv, out_argv_size, "%s", nro_path);
    }
}

bool nro_picker_read_saved_dir(const char* cfg_file, char* out_dir, size_t out_dir_size) {
    if (!cfg_file || !out_dir || out_dir_size == 0) {
        return false;
    }
    out_dir[0] = '\0';

    FILE* f = fopen(cfg_file, "rb");
    if (!f) {
        return false;
    }
    char buf[1024];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) {
        return false;
    }
    buf[n] = '\0';

    char* line_end = strpbrk(buf, "\r\n");
    if (line_end) {
        *line_end = '\0';
    }
    size_t start = 0;
    while (buf[start] != '\0' && isspace((unsigned char)buf[start])) {
        ++start;
    }
    size_t end = strlen(buf);
    while (end > start && isspace((unsigned char)buf[end - 1])) {
        --end;
    }
    while (end > start + 1 && buf[end - 1] == '/') {
        // Preserve "sdmc:/" root if explicitly specified, strip trailing '/' otherwise.
        if (end >= 2 && buf[end - 2] == ':') {
            break;
        }
        --end;
    }
    const size_t len = end - start;
    if (len == 0 || len >= out_dir_size) {
        return false;
    }
    memcpy(out_dir, buf + start, len);
    out_dir[len] = '\0';
    return true;
}
