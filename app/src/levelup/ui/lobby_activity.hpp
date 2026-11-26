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
    /// 掌机/底座切换（UI 缩放变化）后按新的实际像素重解码封面
    void handleDisplayScaleChanged();
    struct CoverTask {
        std::string gameId;
        std::string url;
        uint64_t generation = 0;
        brls::Image* target = nullptr;
        int texMaxW = 320;
        int texMaxH = 180;
    };

    void startCoverWorker();
    void stopCoverWorker();
    void coverWorkerLoop();
    void enqueueCover(const GameEntry& game, uint64_t gen, brls::Image* target, int texMaxW, int texMaxH);

    void updateHeaderUi();
    bool isForegroundActivity() const;
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
    brls::Label* m_modeBadge = nullptr;
    brls::Box* m_searchBox = nullptr;
    brls::Box* m_accountBox = nullptr;
    brls::Label* m_accountLabel = nullptr;
    brls::Label* m_hintLabel = nullptr;
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

    // 大厅预热测速：无实例、无 flow，纯探测，不消耗云游戏时长。
    // 必须用可 join 的线程并在析构时汇合，避免退出时与 curl_global_cleanup 竞争。
    std::thread m_prewarmThread;
    std::shared_ptr<std::atomic<bool>> m_prewarmCancel;
    std::atomic<bool> m_prewarmStarted{false};
    void startDialPrewarm();

    std::optional<GameEntry> m_pendingLaunchGame;

    // 上次重建封面网格时的 UI 缩放（掌机 1.0 / 底座 1.5），用于插拔底座后按新尺寸重解码
    float m_coverTexScale = 0.0f;

    // 窗口尺寸/UI 缩放变化事件订阅（borealis 的 Activity::onWindowSizeChanged 不是虚函数）
    brls::VoidEvent::Subscription m_windowSizeSub;
    bool m_windowSizeSubValid = false;
};

} // namespace levelup::ui
