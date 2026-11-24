#pragma once

#include "levelup/api_client.hpp"
#include "levelup/store.hpp"

#include <borealis.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace levelup::ui {

class LevelUpLobbyActivity : public brls::Activity {
  public:
    LevelUpLobbyActivity();
    ~LevelUpLobbyActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;
    void onResume() override;

  private:
    struct CoverTask {
        std::string gameId;
        std::string url;
        uint64_t generation = 0;
        brls::Image* target = nullptr;
    };

    void startCoverWorker();
    void stopCoverWorker();
    void coverWorkerLoop();
    void enqueueCover(const GameEntry& game, uint64_t gen, brls::Image* target);

    void updateHeaderUi();
    void rebuildCategoryStrip();
    void rebuildGameGrid();
    std::vector<GameEntry> currentFilteredGames() const;

    void openSearchIme();
    void cycleCategory(int delta);
    void changePage(int delta);
    void openGameDetailModal(const GameEntry& game);
    void launchGame(const GameEntry& game);
    void openQrLoginModal();
    void openAccountModal();
    void openSettingsModal();
    void refreshOnlineDataAsync(bool notifyResult);

    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);

    Store m_store;
    AppConfig m_cfg;
    GamesLibrary m_library;
    std::vector<RecentEntry> m_recent;
    api::UserInfo m_userInfo;
    bool m_hasUserInfo = false;

    std::string m_selectedCategory = "__ALL__";
    std::string m_searchQuery;
    size_t m_currentPage = 0;
    size_t m_totalPages = 1;
    static constexpr size_t kGamesPerPage = 24;
    std::atomic<uint64_t> m_gridGeneration{0};
    bool m_needsGridRebuild = false;

    // Owned by Borealis View tree (valid while this Activity is alive)
    brls::Box* m_root = nullptr;
    brls::Label* m_accountLabel = nullptr;
    brls::Label* m_searchChip = nullptr;
    brls::Label* m_countLabel = nullptr;
    brls::Box* m_categoryStrip = nullptr;
    brls::ScrollingFrame* m_scrollFrame = nullptr;

    // Joinable CoverLoader worker (NO detached threads)
    std::thread m_coverThread;
    std::mutex m_coverMu;
    std::condition_variable m_coverCv;
    std::deque<CoverTask> m_coverQueue;
    bool m_coverStop = false;

    // Joinable online refresh & QR login workers
    std::thread m_netThread;
    std::atomic<bool> m_netBusy{false};

    std::thread m_qrThread;
    std::atomic<bool> m_qrCancel{false};

    std::optional<GameEntry> m_pendingLaunchGame;
};

} // namespace levelup::ui
