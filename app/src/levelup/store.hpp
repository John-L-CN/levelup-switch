#pragma once

#include "levelup/api_client.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace levelup {

struct GameEntry {
    int gid = 0;
    std::string gameId;
    std::string name;
    std::string fullName;
    std::string coverUrl;
    std::string description;
    std::vector<std::string> categories;
};

struct CategoryEntry {
    std::string id;
    std::string name;
};

struct RecentEntry {
    std::string gameId;
    int64_t playedAtUnix = 0;
};

struct GamesLibrary {
    std::vector<CategoryEntry> categories;
    std::vector<GameEntry> games;
};

struct AppConfig {
    std::string deviceId;
    api::Credentials cred;
    int videoLevel = 3;          // 0..4 (default 3 = UltraHigh)
    bool vqFirst = true;         // default true (VQ_FIRST)
    bool rumbleEnabled = true;   // default true
    int rumbleStrength = 100;    // 0..100
    bool invertStickY = false;   // default false (libnx +Y is up)
    bool showStatsHud = false;   // default false
    int audioVolume = 100;       // 0..100
};

/// Exact port of go-client MatchGames() search semantics.
std::string normalizeSearchStr(std::string_view s);
std::string compactSearchStr(std::string_view s);
std::vector<GameEntry> matchGames(const std::vector<GameEntry>& games, std::string_view query);

class Store {
  public:
    static constexpr size_t kMaxRecentGames = 12;

    explicit Store(std::string rootDir = "sdmc:/switch/levelup-nx",
                   std::string romfsLibraryPath = "romfs:/levelup/games_library.json");

    const std::string& rootDir() const { return m_rootDir; }
    std::string configPath() const;
    std::string libraryCachePath() const;
    std::string recentPath(int64_t userId) const;
    std::string coversDir() const;
    std::string coverFilePath(std::string_view gameId) const;

    bool ensureDirectories() const;

    AppConfig loadConfig() const;
    bool saveConfig(const AppConfig& cfg) const;

    std::vector<RecentEntry> loadRecent(int64_t userId) const;
    bool saveRecent(int64_t userId, const std::vector<RecentEntry>& list) const;
    std::vector<RecentEntry> recordRecent(int64_t userId, std::string_view gameId, int64_t nowUnix = 0) const;

    GamesLibrary loadLibrary() const;
    bool saveLibrary(const GamesLibrary& lib) const;
    static GamesLibrary mergeOnlineGames(const GamesLibrary& existing, const std::vector<api::GameListItem>& online);

    bool hasCachedCover(std::string_view gameId) const;
    bool loadCoverBytes(std::string_view gameId, std::vector<uint8_t>& outBytes) const;
    bool saveCoverBytes(std::string_view gameId, const std::vector<uint8_t>& bytes) const;
    void clearCoversCache() const;

  private:
    bool readTextFile(const std::string& path, std::string& out) const;
    bool writeTextFileAtomic(const std::string& path, std::string_view content) const;

    std::string m_rootDir;
    std::string m_romfsLibraryPath;
};

} // namespace levelup
