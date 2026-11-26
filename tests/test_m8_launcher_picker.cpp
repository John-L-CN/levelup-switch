// M8: 主界面快捷方式内置引导器 (launcher) 的 NRO 自动选版逻辑单测
//
// 编译（host，Linux）：
//   gcc -std=c11 -c tools/launcher/nro_picker.c -o nro_picker.o
//   g++ -std=c++20 -Itools/launcher tests/test_m8_launcher_picker.cpp nro_picker.o -o t_m8 && ./t_m8
#include "nro_picker.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void writeU32Le(std::vector<uint8_t>& buf, size_t offset, uint32_t v) {
    buf[offset + 0] = static_cast<uint8_t>(v & 0xFFU);
    buf[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    buf[offset + 2] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    buf[offset + 3] = static_cast<uint8_t>((v >> 24) & 0xFFU);
}

void writeU64Le(std::vector<uint8_t>& buf, size_t offset, uint64_t v) {
    for (size_t i = 0; i < 8; ++i) {
        buf[offset + i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFFU);
    }
}

void writeSyntheticNro(const fs::path& p, const std::string& nacpDisplayVersion) {
    constexpr uint32_t kNroSize = 0x80;
    constexpr uint64_t kNacpOffset = 0x38;
    constexpr uint64_t kNacpSize = 0x4000;
    std::vector<uint8_t> data(kNroSize + kNacpOffset + kNacpSize, 0);

    std::memcpy(data.data() + 0x10, "NRO0", 4);
    writeU32Le(data, 0x18, kNroSize);

    std::memcpy(data.data() + kNroSize + 0x00, "ASET", 4);
    writeU32Le(data, kNroSize + 0x04, 0);
    writeU64Le(data, kNroSize + 0x18, kNacpOffset);
    writeU64Le(data, kNroSize + 0x20, kNacpSize);

    if (!nacpDisplayVersion.empty()) {
        const size_t copyLen = std::min<size_t>(nacpDisplayVersion.size(), 15);
        std::memcpy(data.data() + kNroSize + kNacpOffset + 0x3060,
                    nacpDisplayVersion.data(),
                    copyLen);
    }

    std::ofstream out(p, std::ios::binary);
    assert(out.good());
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    assert(out.good());
}

} // namespace

int main() {
    // 1. Semver 解析
    {
        int maj = -1, min = -1, pat = -1;
        assert(nro_picker_parse_semver("LevelUp-NX-v0.3.3.nro", &maj, &min, &pat));
        assert(maj == 0 && min == 3 && pat == 3);

        assert(nro_picker_parse_semver("LevelUp-NX v0.1.8.nro", &maj, &min, &pat));
        assert(maj == 0 && min == 1 && pat == 8);

        assert(nro_picker_parse_semver("v12.4.99", &maj, &min, &pat));
        assert(maj == 12 && min == 4 && pat == 99);

        assert(nro_picker_parse_semver("0.10.2", &maj, &min, &pat));
        assert(maj == 0 && min == 10 && pat == 2);

        assert(!nro_picker_parse_semver("1.2.3.4", &maj, &min, &pat));
        assert(!nro_picker_parse_semver("LevelUp-NX.nro", &maj, &min, &pat));
        assert(!nro_picker_parse_semver("", &maj, &min, &pat));
    }

    // 2. 候选文件名过滤（独占目录 vs 共享 /switch，排除 launcher.nro）
    {
        assert(nro_picker_is_candidate_filename("LevelUp-NX-v0.3.3.nro", false));
        assert(nro_picker_is_candidate_filename("levelup-custom.nro", false));
        assert(!nro_picker_is_candidate_filename("hbmenu.nro", false));
        assert(nro_picker_is_candidate_filename("hbmenu.nro", true));
        assert(!nro_picker_is_candidate_filename("launcher.nro", true));
        assert(!nro_picker_is_candidate_filename("LAUNCHER.NRO", false));
        assert(!nro_picker_is_candidate_filename("levelup-launcher.nro", true));
        assert(!nro_picker_is_candidate_filename("._LevelUp-NX-v0.3.3.nro", true));
        assert(!nro_picker_is_candidate_filename("LevelUp-NX-v0.3.3.txt", true));
    }

    // 3. argv 格式化（含空格加双引号）
    {
        char buf[256];
        nro_picker_format_argv("sdmc:/switch/LevelUp-NX/LevelUp-NX-v0.3.3.nro", buf, sizeof(buf));
        assert(std::string(buf) == "sdmc:/switch/LevelUp-NX/LevelUp-NX-v0.3.3.nro");

        nro_picker_format_argv("sdmc:/switch/LevelUp-NX/LevelUp-NX v0.1.8.nro", buf, sizeof(buf));
        assert(std::string(buf) == "\"sdmc:/switch/LevelUp-NX/LevelUp-NX v0.1.8.nro\"");
    }

    // 4. NRO NACP display_version 读取与目录自动选版
    const fs::path tmpRoot = fs::temp_directory_path() / "levelup_m8_picker_test";
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    const fs::path dedicatedDir = tmpRoot / "LevelUp-NX";
    const fs::path sharedDir = tmpRoot / "switch";
    fs::create_directories(dedicatedDir, ec);
    fs::create_directories(sharedDir, ec);

    {
        const fs::path cfgPath = tmpRoot / "nro_dir.txt";
        std::ofstream cfg(cfgPath, std::ios::binary);
        cfg << "  " << dedicatedDir.string() << "/  \r\nignored\n";
        cfg.close();

        char saved[512];
        assert(nro_picker_read_saved_dir(cfgPath.string().c_str(), saved, sizeof(saved)));
        assert(std::string(saved) == dedicatedDir.string());
    }

    // 场景 A：新旧两版同时存在或升级替换 -> 自动选中最高版本，且忽略 launcher.nro 与无关程序
    writeSyntheticNro(dedicatedDir / "launcher.nro", "9.9.9");
    writeSyntheticNro(dedicatedDir / "LevelUp-NX-v0.3.2.nro", "0.3.2");
    writeSyntheticNro(dedicatedDir / "LevelUp-NX-v0.3.3.nro", "0.3.3");
    writeSyntheticNro(sharedDir / "other-tool-v9.9.9.nro", "9.9.9");
    writeSyntheticNro(sharedDir / "LevelUp-NX-v0.3.1.nro", "0.3.1");

    {
        char nacpVer[32];
        assert(nro_picker_read_nacp_version(
            (dedicatedDir / "LevelUp-NX-v0.3.3.nro").string().c_str(), nacpVer, sizeof(nacpVer)));
        assert(std::string(nacpVer) == "0.3.3");
    }

    const std::string dedStr = dedicatedDir.string();
    const std::string shdStr = sharedDir.string();
    const char* dirs[2] = {dedStr.c_str(), shdStr.c_str()};
    const bool flags[2] = {true, false};
    char picked[1024];

    assert(nro_picker_select_best(dirs, flags, 2, picked, sizeof(picked)));
    assert(fs::path(picked).filename() == "LevelUp-NX-v0.3.3.nro");

    // 场景 B：删掉 v0.3.3，放入无版本号文件名的 v0.3.4（凭 NACP display_version 识别胜出）
    fs::remove(dedicatedDir / "LevelUp-NX-v0.3.3.nro", ec);
    writeSyntheticNro(dedicatedDir / "LevelUp-NX.nro", "0.3.4");
    assert(nro_picker_select_best(dirs, flags, 2, picked, sizeof(picked)));
    assert(fs::path(picked).filename() == "LevelUp-NX.nro");

    // 场景 C：独占目录被删空，但用户在共享 /switch 放了新版 -> 仍能从 /switch 找到并启动
    fs::remove(dedicatedDir / "LevelUp-NX.nro", ec);
    fs::remove(dedicatedDir / "LevelUp-NX-v0.3.2.nro", ec);
    assert(nro_picker_select_best(dirs, flags, 2, picked, sizeof(picked)));
    assert(fs::path(picked).filename() == "LevelUp-NX-v0.3.1.nro");

    // 场景 D：独占目录仅有一个用户改名后的 custom.nro（不以 levelup 开头）-> 兜底选中
    fs::remove(sharedDir / "LevelUp-NX-v0.3.1.nro", ec);
    writeSyntheticNro(dedicatedDir / "custom.nro", "0.3.5");
    assert(nro_picker_select_best(dirs, flags, 2, picked, sizeof(picked)));
    assert(fs::path(picked).filename() == "custom.nro");

    fs::remove_all(tmpRoot, ec);
    std::cout << "test_m8_launcher_picker: all assertions passed\n";
    return 0;
}
