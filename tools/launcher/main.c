#include "nro_picker.h"

#include <stdio.h>
#include <string.h>
#include <switch.h>

static bool is_shared_switch_root(const char* dir) {
    if (!dir) {
        return true;
    }
    return strcmp(dir, "sdmc:/switch") == 0 || strcmp(dir, "/switch") == 0;
}

static bool dir_already_listed(const char* const* dirs, size_t count, const char* cand) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(dirs[i], cand) == 0) {
            return true;
        }
    }
    return false;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    char saved_dir_raw[512];
    char saved_dir[512];
    const char* search_dirs[4];
    bool dedicated_flags[4];
    size_t dir_count = 0;

    if (nro_picker_read_saved_dir("sdmc:/switch/levelup-nx/nro_dir.txt",
                                  saved_dir_raw,
                                  sizeof(saved_dir_raw))) {
        if (strncmp(saved_dir_raw, "sdmc:", 5) == 0) {
            snprintf(saved_dir, sizeof(saved_dir), "%s", saved_dir_raw);
        } else if (saved_dir_raw[0] == '/') {
            snprintf(saved_dir, sizeof(saved_dir), "sdmc:%s", saved_dir_raw);
        } else {
            snprintf(saved_dir, sizeof(saved_dir), "sdmc:/%s", saved_dir_raw);
        }
        search_dirs[dir_count] = saved_dir;
        dedicated_flags[dir_count] = !is_shared_switch_root(saved_dir);
        ++dir_count;
    }

    static const char* const kFallbackDirs[3] = {
        "sdmc:/switch/LevelUp-NX",
        "sdmc:/switch/levelup-nx",
        "sdmc:/switch",
    };
    static const bool kFallbackDedicated[3] = {true, true, false};

    for (size_t i = 0; i < 3; ++i) {
        if (!dir_already_listed(search_dirs, dir_count, kFallbackDirs[i])) {
            search_dirs[dir_count] = kFallbackDirs[i];
            dedicated_flags[dir_count] = kFallbackDedicated[i];
            ++dir_count;
        }
    }

    char best_path[1024];
    if (nro_picker_select_best(search_dirs, dedicated_flags, dir_count,
                               best_path, sizeof(best_path))) {
        char normalized_path[1040];
        if (strncmp(best_path, "sdmc:", 5) == 0) {
            snprintf(normalized_path, sizeof(normalized_path), "%s", best_path);
        } else {
            snprintf(normalized_path, sizeof(normalized_path), "sdmc:%s", best_path);
        }

        if (envHasNextLoad()) {
            char next_argv[1056];
            // Must pass a non-empty argv so sphaira forwarder sees g_nextArgv replaced.
            nro_picker_format_argv(normalized_path, next_argv, sizeof(next_argv));
            if (R_SUCCEEDED(envSetNextLoad(normalized_path, next_argv))) {
                return 0;
            }
        }
    }

    consoleInit(NULL);
    printf("LevelUp-NX Launcher\n");
    printf("===================\n\n");
    if (!envHasNextLoad()) {
        printf("ERROR: envHasNextLoad() is unavailable in this environment.\n");
        printf("Please launch LevelUp-NX directly from hbmenu (Hold R on a game).\n\n");
    } else {
        printf("Could not find any LevelUp-NX NRO file.\n\n");
        printf("Searched directories:\n");
        for (size_t i = 0; i < dir_count; ++i) {
            printf("  - %s\n", search_dirs[i]);
        }
        printf("\nPlease copy LevelUp-NX-vX.Y.Z.nro into:\n");
        printf("  sdmc:/switch/LevelUp-NX/\n");
        printf("or:\n");
        printf("  sdmc:/switch/\n\n");
    }
    printf("Press + or B to return.\n");
    consoleUpdate(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 kDown = padGetButtonsDown(&pad);
        if (kDown & (HidNpadButton_Plus | HidNpadButton_B | HidNpadButton_A)) {
            break;
        }
        consoleUpdate(NULL);
    }

    consoleExit(NULL);
    return 0;
}
