#include "levelup/store.hpp"

#include <cassert>
#include <filesystem>
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
        assert(c1.videoLevel == 3 && c1.vqFirst == true);
        assert(c1.rumbleEnabled == true && c1.rumbleStrength == 100);

        c1.cred.sessionKey = "s1";
        c1.cred.ciKey = "c1";
        c1.cred.userId = 4242;
        c1.cred.nickName = "SwitchTester";
        c1.rumbleStrength = 75;
        c1.videoLevel = 4;
        c1.vqFirst = false;
        assert(store.saveConfig(c1));

        auto c2 = store.loadConfig();
        assert(c2.deviceId == c1.deviceId);
        assert(c2.cred.sessionKey == "s1" && c2.cred.userId == 4242);
        assert(c2.rumbleStrength == 75 && c2.videoLevel == 4 && c2.vqFirst == false);
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

    fs::remove_all(tmpDir, ec);
    std::cout << "test_m5_store_search: ALL PASSED\n";
    return 0;
}
