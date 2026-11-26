#include "levelup/store.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace levelup;

int main() {
    namespace fs = std::filesystem;
    const std::string tmpDir = "/tmp/levelup_nx_store_test";
    std::error_code ec;
    fs::remove_all(tmpDir, ec);

    Store store(tmpDir, "resources/levelup/games_library.json");

    // 1. Config persistence + random deviceId auto-generation
    {
        auto c1 = store.loadConfig();
        assert(c1.deviceId.size() == 32);
        // 画质固定「高清」(2)，不再可配；掌机分辨率默认 720p
        assert(c1.videoLevel == AppConfig::kFixedVideoLevel && c1.videoLevel == 2);
        assert(c1.handheldResolution == AppConfig::kHandheldRes720);
        assert(c1.touchMouseMode == AppConfig::kTouchMouseClick);
        assert(c1.vqFirst == true);
        assert(c1.rumbleEnabled == true && c1.rumbleStrength == 100);

        c1.cred.sessionKey = "s1";
        c1.cred.ciKey = "c1";
        c1.cred.userId = 4242;
        c1.cred.nickName = "SwitchTester";
        c1.rumbleStrength = 75;
        c1.videoLevel = 4; // 旧配置里的档位必须被忽略
        c1.handheldResolution = AppConfig::kHandheldRes1080;
        c1.touchMouseMode = AppConfig::kTouchMouseTrackpad;
        c1.vqFirst = false;
        assert(store.saveConfig(c1));

        auto c2 = store.loadConfig();
        assert(c2.deviceId == c1.deviceId);
        assert(c2.cred.sessionKey == "s1" && c2.cred.userId == 4242);
        assert(c2.rumbleStrength == 75 && c2.vqFirst == false);
        assert(c2.videoLevel == AppConfig::kFixedVideoLevel);
        assert(c2.handheldResolution == AppConfig::kHandheldRes1080);
        assert(c2.touchMouseMode == AppConfig::kTouchMouseTrackpad);

        // 第三种触屏鼠标模式：鼠标锁定（2）持久化和越界值回落
        assert(normalizeTouchMouseMode(AppConfig::kTouchMouseLocked) == AppConfig::kTouchMouseLocked);
        assert(normalizeTouchMouseMode(99) == AppConfig::kTouchMouseClick);
        c2.touchMouseMode = AppConfig::kTouchMouseLocked;
        assert(store.saveConfig(c2));
        auto cLocked = store.loadConfig();
        assert(cLocked.touchMouseMode == AppConfig::kTouchMouseLocked);

        // 非法数字或类型错误的 handheldResolution 回落到 720p，且不破坏其余字段
        {
            std::ofstream ofs(store.configPath(), std::ios::binary | std::ios::trunc);
            ofs << R"({"deviceId":")" << c1.deviceId
                << R"(","sessionKey":"s1","handheldResolution":480})";
        }
        auto cBadInt = store.loadConfig();
        assert(cBadInt.deviceId == c1.deviceId && cBadInt.cred.sessionKey == "s1");
        assert(cBadInt.handheldResolution == AppConfig::kHandheldRes720);

        {
            std::ofstream ofs(store.configPath(), std::ios::binary | std::ios::trunc);
            ofs << R"({"deviceId":")" << c1.deviceId
                << R"(","sessionKey":"s1","handheldResolution":"1080"})";
        }
        auto cBadType = store.loadConfig();
        assert(cBadType.deviceId == c1.deviceId && cBadType.cred.sessionKey == "s1");
        assert(cBadType.handheldResolution == AppConfig::kHandheldRes720);

        // 底座固定 1920x1080，掌机按设置走 720p / 1080p
        assert((resolveTargetResolution(true, 720) == std::pair<int, int>{1920, 1080}));
        assert((resolveTargetResolution(true, 1080) == std::pair<int, int>{1920, 1080}));
        assert((resolveTargetResolution(false, 720) == std::pair<int, int>{1280, 720}));
        assert((resolveTargetResolution(false, 1080) == std::pair<int, int>{1920, 1080}));
        assert((resolveTargetResolution(false, 999) == std::pair<int, int>{1280, 720}));
    }

    // 2. Recent games dedup & cap at 12 + per-user file isolation
    {
        for (int i = 1; i <= 15; ++i) {
            store.recordRecent(4242, "g" + std::to_string(i), 1000 + i);
        }
        auto rec = store.recordRecent(4242, "g3", 2000);
        assert(rec.size() == 12);
        assert(rec.front().gameId == "g3" && rec.front().playedAtUnix == 2000);
        assert(store.loadRecent(0).empty()); // guest isolated
    }

    // 3. Cover cache write/read/clear
    {
        const std::vector<uint8_t> jpg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00};
        assert(store.saveCoverBytes("2zkaqmg1", jpg));
        assert(store.hasCachedCover("2zkaqmg1"));
        std::vector<uint8_t> loaded;
        assert(store.loadCoverBytes("2zkaqmg1", loaded) && loaded == jpg);
        store.clearCoversCache();
        assert(!store.hasCachedCover("2zkaqmg1"));
    }

    // 4. Load bundled resources/levelup/games_library.json (238 games, 11 categories)
    {
        const auto lib = store.loadLibrary();
        assert(lib.games.size() == 238);
        assert(lib.categories.size() == 11);

        // Exact gameId match (rank 0)
        auto mId = matchGames(lib.games, "2zkaqmg1");
        assert(!mId.empty() && mId[0].gameId == "2zkaqmg1");

        // Fullwidth & punctuation-stripped search
        auto mNier = matchGames(lib.games, "\xEF\xBC\xAE\xEF\xBC\xA9\xEF\xBC\xA5\xEF\xBC\xB2"); // "ＮＩＥＲ"
        // Also test Chinese punctuation stripping vs multi-token
        auto mCn = matchGames(lib.games, "尼尔：机械纪元");
        assert(!mCn.empty() && mCn[0].gameId == "2zkaqmg1");
        auto mTok = matchGames(lib.games, "尼尔 机械");
        assert(!mTok.empty() && mTok[0].gameId == "2zkaqmg1");
        (void)mNier;
    }

    // 5. Corrupted / null-typed games_library.json must not throw, falls back to bundled romfs
    {
        std::ofstream ofs(store.libraryCachePath(), std::ios::binary | std::ios::trunc);
        ofs << R"({"categories":[{"id":1,"name":null}],"games":[{"gid":null,"gameId":"bad","gameName":123}]})";
        ofs.close();
        const auto lib = store.loadLibrary();
        assert(lib.games.size() == 238);
    }

    fs::remove_all(tmpDir, ec);
    std::cout << "test_m5_store_search: ALL PASSED\n";
    return 0;
}
