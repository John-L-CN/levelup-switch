#pragma once

#include "levelup/api_client.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
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
    /// 画质档位不再暴露给用户：固定"高清"（云端口径 0=流畅 1=标清 2=高清 3=超清 4=原画）。
    static constexpr int kFixedVideoLevel = 2;
    /// 掌机模式可选的流分辨率高度（底座模式始终固定 1080p）。
    static constexpr int kHandheldRes720 = 720;
    static constexpr int kHandheldRes1080 = 1080;
    /// 串流触屏鼠标模式：0=点击模式（绝对坐标直点），1=触控板模式（相对移动+轻触点击）。
    static constexpr int kTouchMouseClick = 0;
    static constexpr int kTouchMouseTrackpad = 1;

    std::string deviceId;
    api::Credentials cred;
    // union 选址用：LP 侧用户号（apply 响应 body.uuid）与最近一次开过的游戏 pkgName。
    // 两者都是大厅预热测速（/v1/speed/dialpoints）的必要参数，学到后持久化。
    std::string lpUuid;
    int64_t lastPkgName = 0;
    int videoLevel = kFixedVideoLevel; // 固定值，见 kFixedVideoLevel
    int handheldResolution = kHandheldRes720; // 720 或 1080；仅掌机模式生效，底座固定 1080p
    bool vqFirst = true;         // default true (VQ_FIRST)
    bool rumbleEnabled = true;   // default true
    int rumbleStrength = 100;    // 0..100
    bool invertStickY = false;   // default false (libnx +Y is up)
    bool swapAbXy = false;       // default false (true = Xbox layout: A<->B, X<->Y)
    bool showStatsHud = false;   // default false
    int audioVolume = 100;       // 0..100
    int touchMouseMode = kTouchMouseClick;
};

/// Exact port of go-client MatchGames() search semantics.
std::string normalizeSearchStr(std::string_view s);
std::string compactSearchStr(std::string_view s);
std::vector<GameEntry> matchGames(const std::vector<GameEntry>& games, std::string_view query);

/// 归一化掌机分辨率高度（仅接受 720 / 1080，其余值回落为 720）。
int normalizeHandheldResolution(int res);

/// 归一化触屏鼠标模式（仅接受 0=点击 / 1=触控板，其余值回落为 0）。
int normalizeTouchMouseMode(int mode);

/// 根据是否底座模式与掌机分辨率设置，决定向云端申请的目标分辨率（底座固定 1920x1080）。
std::pair<int, int> resolveTargetResolution(bool docked, int handheldResolution);

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
