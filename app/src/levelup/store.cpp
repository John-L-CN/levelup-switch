#include "levelup/store.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/log.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace fs = std::filesystem;

namespace levelup {
namespace {

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7FU) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFU) {
        out.push_back(static_cast<char>(0xC0U | ((cp >> 6U) & 0x1FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0xFFFFU) {
        out.push_back(static_cast<char>(0xE0U | ((cp >> 12U) & 0x0FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | ((cp >> 18U) & 0x07U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

uint32_t nextUtf8Codepoint(std::string_view s, size_t& i) {
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80U) {
        ++i;
        return b0;
    }
    if ((b0 & 0xE0U) == 0xC0U && i + 1 < s.size()) {
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        if ((b1 & 0xC0U) == 0x80U) {
            i += 2;
            return (static_cast<uint32_t>(b0 & 0x1FU) << 6U) | static_cast<uint32_t>(b1 & 0x3FU);
        }
    }
    if ((b0 & 0xF0U) == 0xE0U && i + 2 < s.size()) {
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        if ((b1 & 0xC0U) == 0x80U && (b2 & 0xC0U) == 0x80U) {
            i += 3;
            return (static_cast<uint32_t>(b0 & 0x0FU) << 12U) |
                   (static_cast<uint32_t>(b1 & 0x3FU) << 6U) |
                   static_cast<uint32_t>(b2 & 0x3FU);
        }
    }
    if ((b0 & 0xF8U) == 0xF0U && i + 3 < s.size()) {
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        const unsigned char b3 = static_cast<unsigned char>(s[i + 3]);
        if ((b1 & 0xC0U) == 0x80U && (b2 & 0xC0U) == 0x80U && (b3 & 0xC0U) == 0x80U) {
            i += 4;
            return (static_cast<uint32_t>(b0 & 0x07U) << 18U) |
                   (static_cast<uint32_t>(b1 & 0x3FU) << 12U) |
                   (static_cast<uint32_t>(b2 & 0x3FU) << 6U) |
                   static_cast<uint32_t>(b3 & 0x3FU);
        }
    }
    ++i;
    return b0;
}

std::string trimSpaces(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::vector<std::string> splitWhitespace(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) break;
        const size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string sanitizeFilenameComponent(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "unknown" : out;
}

GamesLibrary parseLibraryJson(std::string_view raw) {
    GamesLibrary lib;
    if (raw.empty()) return lib;
    try {
        const nlohmann::json j = nlohmann::json::parse(raw.begin(), raw.end());
        if (!j.is_object()) return lib;

        if (j.contains("categories") && j["categories"].is_array()) {
            for (const auto& c : j["categories"]) {
                if (!c.is_object()) continue;
                CategoryEntry ce;
                if (c.contains("id")) {
                    if (c["id"].is_string()) ce.id = c["id"].get<std::string>();
                    else if (c["id"].is_number_integer()) ce.id = std::to_string(c["id"].get<int64_t>());
                }
                if (c.contains("name") && c["name"].is_string()) {
                    ce.name = c["name"].get<std::string>();
                }
                if (!ce.name.empty()) {
                    lib.categories.push_back(std::move(ce));
                }
            }
        }

        if (j.contains("games") && j["games"].is_array()) {
            for (const auto& g : j["games"]) {
                if (!g.is_object()) continue;
                GameEntry ge;
                if (g.contains("gid") && g["gid"].is_number_integer()) {
                    ge.gid = g["gid"].get<int>();
                }
                if (g.contains("gameId") && g["gameId"].is_string()) {
                    ge.gameId = g["gameId"].get<std::string>();
                }
                if (g.contains("gameName") && g["gameName"].is_string()) {
                    ge.name = g["gameName"].get<std::string>();
                }
                if (g.contains("gameFullName") && g["gameFullName"].is_string()) {
                    ge.fullName = g["gameFullName"].get<std::string>();
                }
                if (g.contains("coverUrl") && g["coverUrl"].is_string()) {
                    ge.coverUrl = g["coverUrl"].get<std::string>();
                }
                if (g.contains("description") && g["description"].is_string()) {
                    ge.description = g["description"].get<std::string>();
                }
                if (g.contains("categories") && g["categories"].is_array()) {
                    for (const auto& cat : g["categories"]) {
                        if (cat.is_string()) {
                            ge.categories.push_back(cat.get<std::string>());
                        } else if (cat.is_object() && cat.contains("name") && cat["name"].is_string()) {
                            ge.categories.push_back(cat["name"].get<std::string>());
                        }
                    }
                }
                if (!ge.gameId.empty()) {
                    lib.games.push_back(std::move(ge));
                }
            }
        }
    } catch (const std::exception& e) {
        logWarn(std::string("[Store] library JSON parse failed: ") + e.what());
        return GamesLibrary{};
    }
    return lib;
}

} // namespace

std::string normalizeSearchStr(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = nextUtf8Codepoint(s, i);
        if (cp >= 0xFF01U && cp <= 0xFF5EU) {
            cp -= 0xFEE0U;
        } else if (cp == 0x3000U) {
            cp = ' ';
        }
        if (cp >= 'A' && cp <= 'Z') {
            cp += ('a' - 'A');
        }
        appendUtf8(out, cp);
    }
    return out;
}

std::string compactSearchStr(std::string_view s) {
    const std::string norm = normalizeSearchStr(s);
    std::string out;
    out.reserve(norm.size());
    size_t i = 0;
    while (i < norm.size()) {
        const uint32_t cp = nextUtf8Codepoint(norm, i);
        switch (cp) {
        case ' ':
        case '\t':
        case '\r':
        case '\n':
        case 0x2122U: // ™
        case 0x00AEU: // ®
        case ':':
        case 0xFF1AU: // ：
        case '-':
        case '_':
        case 0x00B7U: // ·
        case 0x30FBU: // ・
        case '/':
            continue;
        default:
            appendUtf8(out, cp);
            break;
        }
    }
    return out;
}

std::vector<GameEntry> matchGames(const std::vector<GameEntry>& games, std::string_view query) {
    const std::string q = trimSpaces(normalizeSearchStr(query));
    if (q.empty()) {
        return games;
    }
    const auto tokens = splitWhitespace(q);
    if (tokens.empty()) {
        return games;
    }

    std::vector<std::string> compactTokens;
    compactTokens.reserve(tokens.size());
    for (const auto& tok : tokens) {
        compactTokens.push_back(compactSearchStr(tok));
    }
    const std::string qCompact = compactSearchStr(q);

    struct Scored {
        const GameEntry* game = nullptr;
        int rank = 0;
    };
    std::vector<Scored> hits;

    for (const auto& g : games) {
        const std::string nameLower = normalizeSearchStr(g.name);
        const std::string fullLower = normalizeSearchStr(g.fullName);
        const std::string nameComp = compactSearchStr(g.name);
        const std::string fullComp = compactSearchStr(g.fullName);

        const std::string joinedLower = nameLower + " " + fullLower;
        const std::string joinedComp = nameComp + " " + fullComp;

        int rank = -1;
        if (equalsIgnoreCaseAscii(g.gameId, q) ||
            (!qCompact.empty() &&
             (nameComp.rfind(qCompact, 0) == 0 || fullComp.rfind(qCompact, 0) == 0))) {
            rank = 0;
        } else {
            bool allMatch = true;
            for (size_t i = 0; i < tokens.size(); ++i) {
                const bool rawOk = joinedLower.find(tokens[i]) != std::string::npos;
                const bool compOk = !compactTokens[i].empty() &&
                                    joinedComp.find(compactTokens[i]) != std::string::npos;
                if (!rawOk && !compOk) {
                    allMatch = false;
                    break;
                }
            }
            if (allMatch) {
                rank = 1;
            }
        }

        if (rank >= 0) {
            hits.push_back(Scored{&g, rank});
        }
    }

    std::stable_sort(hits.begin(), hits.end(),
                     [](const Scored& a, const Scored& b) { return a.rank < b.rank; });

    std::vector<GameEntry> out;
    out.reserve(hits.size());
    for (const auto& h : hits) {
        out.push_back(*h.game);
    }
    return out;
}

int normalizeHandheldResolution(int res) {
    return res == AppConfig::kHandheldRes1080 ? AppConfig::kHandheldRes1080
                                              : AppConfig::kHandheldRes720;
}

int normalizeTouchMouseMode(int mode) {
    if (mode == AppConfig::kTouchMouseTrackpad || mode == AppConfig::kTouchMouseLocked) {
        return mode;
    }
    return AppConfig::kTouchMouseClick;
}

std::pair<int, int> resolveTargetResolution(bool docked, int handheldResolution) {
    if (docked || normalizeHandheldResolution(handheldResolution) == AppConfig::kHandheldRes1080) {
        return {1920, 1080};
    }
    return {1280, 720};
}

Store::Store(std::string rootDir, std::string romfsLibraryPath)
    : m_rootDir(std::move(rootDir)),
      m_romfsLibraryPath(std::move(romfsLibraryPath)) {
    while (m_rootDir.size() > 1 && m_rootDir.back() == '/') {
        m_rootDir.pop_back();
    }
}

std::string Store::configPath() const { return m_rootDir + "/config.json"; }
std::string Store::libraryCachePath() const { return m_rootDir + "/games_library.json"; }
std::string Store::recentPath(int64_t userId) const {
    const std::string key = userId > 0 ? std::to_string(userId) : "guest";
    return m_rootDir + "/recent_" + key + ".json";
}
std::string Store::coversDir() const { return m_rootDir + "/covers"; }
std::string Store::coverFilePath(std::string_view gameId) const {
    return coversDir() + "/" + sanitizeFilenameComponent(gameId) + ".jpg";
}

bool Store::ensureDirectories() const {
    std::error_code ec;
    fs::create_directories(m_rootDir, ec);
    if (ec) return false;
    fs::create_directories(coversDir(), ec);
    return !ec;
}

bool Store::readTextFile(const std::string& path, std::string& out) const {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    ifs.seekg(0, std::ios::end);
    const auto sz = ifs.tellg();
    if (sz < 0) return false;
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) {
        ifs.read(out.data(), sz);
    }
    return static_cast<bool>(ifs) || ifs.eof();
}

bool Store::writeTextFileAtomic(const std::string& path, std::string_view content) const {
    ensureDirectories();
    const std::string tmp = path + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        if (!content.empty()) {
            ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        ofs.flush();
        if (!ofs) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
    }
    return !ec;
}

AppConfig Store::loadConfig() const {
    AppConfig cfg;
    std::string raw;
    if (readTextFile(configPath(), raw) && !raw.empty()) {
        try {
            const auto j = nlohmann::json::parse(raw);
            if (j.is_object()) {
                cfg.deviceId = j.value("deviceId", "");
                cfg.cred.sessionKey = j.value("sessionKey", "");
                cfg.cred.ciKey = j.value("ciKey", "");
                cfg.cred.secretKey = j.value("secretKey", "");
                cfg.cred.userId = j.value("userId", int64_t{0});
                cfg.cred.nickName = j.value("nickName", "");
                cfg.cred.mobile = j.value("mobile", "");
                cfg.lpUuid = j.value("lpUuid", "");
                cfg.lastPkgName = j.value("lastPkgName", int64_t{0});
                const int vlOv = j.value("vlOverride", 0);
                cfg.videoLevelOverride = (vlOv == 2 || vlOv == 3) ? vlOv : 0;
                int hhRes = AppConfig::kHandheldRes720;
                if (j.contains("handheldResolution") && j["handheldResolution"].is_number_integer()) {
                    hhRes = j["handheldResolution"].get<int>();
                }
                cfg.handheldResolution = normalizeHandheldResolution(hhRes);
                cfg.vqFirst = j.value("vqFirst", true);
                cfg.rumbleEnabled = j.value("rumbleEnabled", true);
                cfg.rumbleStrength = std::clamp(j.value("rumbleStrength", 100), 0, 100);
                cfg.invertStickY = j.value("invertStickY", false);
                cfg.swapAbXy = j.value("swapAbXy", false);
                cfg.showStatsHud = j.value("showStatsHud", false);
                cfg.audioVolume = std::clamp(j.value("audioVolume", 100), 0, 100);
                int tmMode = AppConfig::kTouchMouseClick;
                if (j.contains("touchMouseMode") && j["touchMouseMode"].is_number_integer()) {
                    tmMode = j["touchMouseMode"].get<int>();
                }
                cfg.touchMouseMode = normalizeTouchMouseMode(tmMode);
            }
        } catch (const std::exception& e) {
            logWarn(std::string("[Store] config.json parse failed: ") + e.what());
        }
    }

    if (cfg.deviceId.empty()) {
        cfg.deviceId = randomHex32();
        saveConfig(cfg);
    }
    return cfg;
}

bool Store::saveConfig(const AppConfig& cfg) const {
    const nlohmann::json j = {
        {"deviceId", cfg.deviceId.empty() ? randomHex32() : cfg.deviceId},
        {"sessionKey", cfg.cred.sessionKey},
        {"ciKey", cfg.cred.ciKey},
        {"secretKey", cfg.cred.secretKey},
        {"userId", cfg.cred.userId},
        {"nickName", cfg.cred.nickName},
        {"mobile", cfg.cred.mobile},
        {"lpUuid", cfg.lpUuid},
        {"lastPkgName", cfg.lastPkgName},
        {"vlOverride", (cfg.videoLevelOverride == 2 || cfg.videoLevelOverride == 3) ? cfg.videoLevelOverride : 0},
        {"handheldResolution", normalizeHandheldResolution(cfg.handheldResolution)},
        {"vqFirst", cfg.vqFirst},
        {"rumbleEnabled", cfg.rumbleEnabled},
        {"rumbleStrength", std::clamp(cfg.rumbleStrength, 0, 100)},
        {"invertStickY", cfg.invertStickY},
        {"swapAbXy", cfg.swapAbXy},
        {"showStatsHud", cfg.showStatsHud},
        {"audioVolume", std::clamp(cfg.audioVolume, 0, 100)},
        {"touchMouseMode", normalizeTouchMouseMode(cfg.touchMouseMode)},
    };
    return writeTextFileAtomic(configPath(), j.dump(2));
}

std::vector<RecentEntry> Store::loadRecent(int64_t userId) const {
    std::vector<RecentEntry> out;
    std::string raw;
    if (!readTextFile(recentPath(userId), raw) || raw.empty()) {
        return out;
    }
    try {
        const auto j = nlohmann::json::parse(raw);
        if (j.is_array()) {
            for (const auto& item : j) {
                if (!item.is_object()) continue;
                RecentEntry e;
                e.gameId = item.value("gameId", "");
                e.playedAtUnix = item.value("at", int64_t{0});
                if (!e.gameId.empty()) {
                    out.push_back(std::move(e));
                }
            }
        }
    } catch (...) {
    }
    if (out.size() > kMaxRecentGames) {
        out.resize(kMaxRecentGames);
    }
    return out;
}

bool Store::saveRecent(int64_t userId, const std::vector<RecentEntry>& list) const {
    nlohmann::json arr = nlohmann::json::array();
    const size_t n = std::min(list.size(), kMaxRecentGames);
    for (size_t i = 0; i < n; ++i) {
        arr.push_back({{"gameId", list[i].gameId}, {"at", list[i].playedAtUnix}});
    }
    return writeTextFileAtomic(recentPath(userId), arr.dump(2));
}

std::vector<RecentEntry> Store::recordRecent(int64_t userId,
                                             std::string_view gameId,
                                             int64_t nowUnix) const {
    auto list = loadRecent(userId);
    list.erase(
        std::remove_if(list.begin(), list.end(),
                       [&](const RecentEntry& e) { return e.gameId == gameId; }),
        list.end());

    const int64_t ts = nowUnix > 0 ? nowUnix : unixSecondsNow();
    list.insert(list.begin(), RecentEntry{std::string(gameId), ts});
    if (list.size() > kMaxRecentGames) {
        list.resize(kMaxRecentGames);
    }
    saveRecent(userId, list);
    return list;
}

GamesLibrary Store::loadLibrary() const {
    std::string raw;
    if (readTextFile(libraryCachePath(), raw)) {
        auto lib = parseLibraryJson(raw);
        if (!lib.games.empty()) {
            return lib;
        }
    }
    if (!m_romfsLibraryPath.empty() && readTextFile(m_romfsLibraryPath, raw)) {
        auto lib = parseLibraryJson(raw);
        if (!lib.games.empty()) {
            return lib;
        }
    }
    return GamesLibrary{};
}

bool Store::saveLibrary(const GamesLibrary& lib) const {
    nlohmann::json cats = nlohmann::json::array();
    for (const auto& c : lib.categories) {
        cats.push_back({{"id", c.id}, {"name", c.name}});
    }
    nlohmann::json games = nlohmann::json::array();
    for (const auto& g : lib.games) {
        games.push_back({
            {"gid", g.gid},
            {"gameId", g.gameId},
            {"gameName", g.name},
            {"gameFullName", g.fullName},
            {"coverUrl", g.coverUrl},
            {"description", g.description},
            {"categories", g.categories},
        });
    }
    const nlohmann::json root = {{"categories", cats}, {"games", games}};
    return writeTextFileAtomic(libraryCachePath(), root.dump());
}

GamesLibrary Store::mergeOnlineGames(const GamesLibrary& existing,
                                     const std::vector<api::GameListItem>& online) {
    GamesLibrary out;
    std::unordered_set<std::string> seenCats;
    for (const auto& c : existing.categories) {
        if (!c.name.empty() && seenCats.insert(c.name).second) {
            out.categories.push_back(c);
        }
    }

    std::unordered_set<std::string> seenGameIds;
    for (const auto& og : online) {
        if (og.gameId.empty() || !seenGameIds.insert(og.gameId).second) continue;
        GameEntry ge;
        ge.gid = og.gid;
        ge.gameId = og.gameId;
        ge.name = og.gameName;
        ge.fullName = og.gameFullName.empty() ? og.gameName : og.gameFullName;
        ge.coverUrl = og.coverUrl;
        ge.description = og.description;
        for (const auto& c : og.categories) {
            if (!c.name.empty()) {
                ge.categories.push_back(c.name);
                if (seenCats.insert(c.name).second) {
                    out.categories.push_back(CategoryEntry{c.id, c.name});
                }
            }
        }
        out.games.push_back(std::move(ge));
    }
    for (const auto& eg : existing.games) {
        if (!eg.gameId.empty() && seenGameIds.insert(eg.gameId).second) {
            out.games.push_back(eg);
        }
    }
    return out;
}

bool Store::hasCachedCover(std::string_view gameId) const {
    std::error_code ec;
    return fs::is_regular_file(coverFilePath(gameId), ec) &&
           fs::file_size(coverFilePath(gameId), ec) > 0;
}

bool Store::loadCoverBytes(std::string_view gameId, std::vector<uint8_t>& outBytes) const {
    std::ifstream ifs(coverFilePath(gameId), std::ios::binary);
    if (!ifs.is_open()) return false;
    ifs.seekg(0, std::ios::end);
    const auto sz = ifs.tellg();
    if (sz <= 0) return false;
    ifs.seekg(0, std::ios::beg);
    outBytes.resize(static_cast<size_t>(sz));
    ifs.read(reinterpret_cast<char*>(outBytes.data()), sz);
    return static_cast<bool>(ifs);
}

bool Store::saveCoverBytes(std::string_view gameId, const std::vector<uint8_t>& bytes) const {
    if (bytes.empty()) return false;
    ensureDirectories();
    const std::string path = coverFilePath(gameId);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        ofs.flush();
        if (!ofs) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
    }
    return !ec;
}

void Store::clearCoversCache() const {
    std::error_code ec;
    fs::remove_all(coversDir(), ec);
    ensureDirectories();
}

} // namespace levelup
