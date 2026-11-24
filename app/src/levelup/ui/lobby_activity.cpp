#include "levelup/ui/lobby_activity.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/log.hpp"
#include "levelup/ui/levelup_stream_view.hpp"

#include <borealis/core/touch/tap_gesture.hpp>
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>

#ifdef __SWITCH__
#include <forwarder_maker.hpp>
#include <switch.h>
#endif

namespace levelup::ui {

namespace {

class TranslucentModalActivity : public brls::Activity {
  public:
    explicit TranslucentModalActivity(brls::View* view) : brls::Activity(view) {}

    bool isTranslucent() override {
        return true;
    }

    float getShowAnimationDuration(brls::TransitionAnimation) override {
        return 0.0f;
    }
};

// Borealis Box::draw() skips culling for Box children, which overflows deko3d's
// 128 KB DynamicCmdSize command buffer when many game card Boxes live inside a ScrollingFrame.
class CulledGridBox : public brls::Box {
  public:
    CulledGridBox() : brls::Box(brls::Axis::COLUMN) {}

    void draw(NVGcontext* vg,
              float x,
              float y,
              float width,
              float height,
              brls::Style style,
              brls::FrameContext* ctx) override {
        brls::Box* parent = this->getParent();
        if (!parent) {
            brls::Box::draw(vg, x, y, width, height, style, ctx);
            return;
        }
        (void)vg;
        (void)x;
        (void)y;
        (void)width;
        (void)height;
        (void)style;

        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        float left = 0.0f;
        parent->getCullingBounds(&top, &right, &bottom, &left);
        top -= 64.0f;
        bottom += 64.0f;

        for (brls::View* child : this->getChildren()) {
            const float childTop = child->getY();
            const float childBottom = childTop + child->getHeight();
            if (childBottom < top || childTop > bottom) {
                continue;
            }
            child->frame(ctx);
        }
    }
};

std::mutex& stbiDecodeMutex() {
    static std::mutex mu;
    return mu;
}

bool decodeDownscaleToRgba(const uint8_t* data,
                           size_t size,
                           int maxW,
                           int maxH,
                           std::vector<uint8_t>& outRgba,
                           int& outW,
                           int& outH) {
    outRgba.clear();
    outW = 0;
    outH = 0;
    if (!data || size == 0 || size > 16 * 1024 * 1024 || maxW <= 0 || maxH <= 0) {
        return false;
    }

    int srcW = 0;
    int srcH = 0;
    int comp = 0;
    stbi_uc* pixels = nullptr;
    {
        // Borealis Switch builds define -DSTBI_NO_THREAD_LOCALS; serialize stb_image calls.
        std::lock_guard lock(stbiDecodeMutex());
        if (!stbi_info_from_memory(data, static_cast<int>(size), &srcW, &srcH, &comp)) {
            return false;
        }
        if (srcW <= 0 || srcH <= 0 || srcW > 4096 || srcH > 4096) {
            return false;
        }
        pixels = stbi_load_from_memory(data, static_cast<int>(size), &srcW, &srcH, &comp, 4);
    }
    if (!pixels) {
        return false;
    }

    if (srcW <= maxW && srcH <= maxH) {
        outW = srcW;
        outH = srcH;
        outRgba.resize(static_cast<size_t>(srcW) * static_cast<size_t>(srcH) * 4);
        std::memcpy(outRgba.data(), pixels, outRgba.size());
        {
            std::lock_guard lock(stbiDecodeMutex());
            stbi_image_free(pixels);
        }
        return true;
    }

    const double scaleX = static_cast<double>(maxW) / static_cast<double>(srcW);
    const double scaleY = static_cast<double>(maxH) / static_cast<double>(srcH);
    const double scale = std::min(scaleX, scaleY);
    const int dstW = std::max(1, static_cast<int>(std::lround(srcW * scale)));
    const int dstH = std::max(1, static_cast<int>(std::lround(srcH * scale)));

    outRgba.resize(static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4);
    for (int dy = 0; dy < dstH; ++dy) {
        const double fy = dstH > 1
                              ? (static_cast<double>(dy) + 0.5) * static_cast<double>(srcH) /
                                        static_cast<double>(dstH) -
                                    0.5
                              : 0.0;
        const double cy = std::clamp(fy, 0.0, static_cast<double>(srcH - 1));
        const int y0 = static_cast<int>(std::floor(cy));
        const int y1 = std::min(y0 + 1, srcH - 1);
        const double wy = cy - static_cast<double>(y0);

        for (int dx = 0; dx < dstW; ++dx) {
            const double fx = dstW > 1
                                  ? (static_cast<double>(dx) + 0.5) * static_cast<double>(srcW) /
                                            static_cast<double>(dstW) -
                                        0.5
                                  : 0.0;
            const double cx = std::clamp(fx, 0.0, static_cast<double>(srcW - 1));
            const int x0 = static_cast<int>(std::floor(cx));
            const int x1 = std::min(x0 + 1, srcW - 1);
            const double wx = cx - static_cast<double>(x0);

            const uint8_t* p00 = pixels + (static_cast<size_t>(y0) * srcW + x0) * 4;
            const uint8_t* p10 = pixels + (static_cast<size_t>(y0) * srcW + x1) * 4;
            const uint8_t* p01 = pixels + (static_cast<size_t>(y1) * srcW + x0) * 4;
            const uint8_t* p11 = pixels + (static_cast<size_t>(y1) * srcW + x1) * 4;

            uint8_t* dst = outRgba.data() + (static_cast<size_t>(dy) * dstW + dx) * 4;
            for (int c = 0; c < 4; ++c) {
                const double topVal = p00[c] * (1.0 - wx) + p10[c] * wx;
                const double botVal = p01[c] * (1.0 - wx) + p11[c] * wx;
                const double val = topVal * (1.0 - wy) + botVal * wy;
                dst[c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(val)), 0, 255));
            }
        }
    }

    {
        std::lock_guard lock(stbiDecodeMutex());
        stbi_image_free(pixels);
    }
    outW = dstW;
    outH = dstH;
    return true;
}

void applyRgbaTexture(brls::Image* img, const std::vector<uint8_t>& rgba, int w, int h) {
    if (!img || w <= 0 || h <= 0 ||
        rgba.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
        return;
    }
    NVGcontext* vg = brls::Application::getNVGContext();
    if (!vg) {
        return;
    }
    const int tex = nvgCreateImageRGBA(vg, w, h, 0, rgba.data());
    if (tex > 0) {
        img->innerSetImage(tex);
    }
}

std::string truncateUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) {
        return s;
    }
    size_t end = maxBytes > 3 ? maxBytes - 3 : maxBytes;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return s.substr(0, end) + "...";
}

std::string joinGameCategories(const std::vector<std::string>& cats) {
    if (cats.empty()) {
        return "云游戏";
    }
    std::string out;
    const size_t limit = std::min<size_t>(cats.size(), 2);
    for (size_t i = 0; i < limit; ++i) {
        if (i > 0) {
            out += " · ";
        }
        out += cats[i];
    }
    return out;
}

std::string detectDisplayBadge() {
#ifdef __SWITCH__
    if (appletGetOperationMode() == AppletOperationMode_Console) {
        return "底座 1080p60";
    }
    return "掌机 720p60";
#else
    return "桌面 720p60";
#endif
}

int percentToFiveStepIndex(int pct) {
    static const int kSteps[5] = {0, 25, 50, 75, 100};
    int bestIdx = 4;
    int bestDiff = 9999;
    for (int i = 0; i < 5; ++i) {
        const int diff = std::abs(pct - kSteps[i]);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestIdx = i;
        }
    }
    return bestIdx;
}

int fiveStepIndexToPercent(int idx) {
    static const int kSteps[5] = {0, 25, 50, 75, 100};
    return kSteps[std::clamp(idx, 0, 4)];
}

} // namespace

LevelUpLobbyActivity::LevelUpLobbyActivity() = default;

LevelUpLobbyActivity::~LevelUpLobbyActivity() {
    m_alive->store(false);
    m_qrCancel.store(true);
    stopCoverWorker();
    if (m_qrThread.joinable()) {
        m_qrThread.join();
    }
    if (m_netThread.joinable()) {
        m_netThread.join();
    }
}

brls::View* LevelUpLobbyActivity::createContentView() {
    m_store.ensureDirectories();
    m_cfg = m_store.loadConfig();
    if (m_cfg.deviceId.empty()) {
        m_cfg.deviceId = randomHex32();
        m_store.saveConfig(m_cfg);
    }
    m_library = m_store.loadLibrary();
    m_recent = m_store.loadRecent(m_cfg.cred.userId);

    m_root = new brls::Box(brls::Axis::COLUMN);
    m_root->setBackgroundColor(nvgRGB(15, 21, 33));
    m_root->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_root->setAlignItems(brls::AlignItems::STRETCH);

#ifdef __SWITCH__
    const AppletType appletType = appletGetAppletType();
    if (appletType != AppletType_Application && appletType != AppletType_SystemApplication) {
        auto* banner = new brls::Box(brls::Axis::ROW);
        banner->setBackgroundColor(nvgRGB(180, 105, 16));
        banner->setPadding(7.0f, 24.0f, 7.0f, 24.0f);
        banner->setJustifyContent(brls::JustifyContent::CENTER);

        auto* warn = new brls::Label();
        warn->setText(
            "警告：当前为相册模式(低内存，硬解易崩溃)。请退回桌面按住 R 打开任意游戏以满血模式启动！");
        warn->setFontSize(15.0f);
        warn->setTextColor(nvgRGB(255, 255, 255));
        banner->addView(warn);
        m_root->addView(banner);
    }
#endif

    // Top Header Bar
    auto* header = new brls::Box(brls::Axis::ROW);
    header->setHeight(62.0f);
    header->setBackgroundColor(nvgRGB(22, 30, 46));
    header->setPadding(0.0f, 24.0f, 0.0f, 24.0f);
    header->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    header->setAlignItems(brls::AlignItems::CENTER);

    auto* leftGroup = new brls::Box(brls::Axis::ROW);
    leftGroup->setAlignItems(brls::AlignItems::CENTER);

    auto* brandLabel = new brls::Label();
    brandLabel->setText("LevelUp-NX");
    brandLabel->setFontSize(26.0f);
    brandLabel->setTextColor(nvgRGB(248, 250, 252));
    brandLabel->setMarginRight(14.0f);
    leftGroup->addView(brandLabel);

    auto* modeBadge = new brls::Label();
    modeBadge->setText(detectDisplayBadge());
    modeBadge->setFontSize(15.0f);
    modeBadge->setTextColor(nvgRGB(56, 189, 248));
    modeBadge->setMarginRight(14.0f);
    leftGroup->addView(modeBadge);

    m_searchBox = new brls::Box(brls::Axis::ROW);
    m_searchBox->setHeight(34.0f);
    m_searchBox->setPadding(0.0f, 14.0f, 0.0f, 14.0f);
    m_searchBox->setCornerRadius(17.0f);
    m_searchBox->setHighlightCornerRadius(17.0f);
    m_searchBox->setBackgroundColor(nvgRGB(30, 41, 59));
    m_searchBox->setAlignItems(brls::AlignItems::CENTER);
    m_searchBox->setFocusable(true);
    m_searchBox->registerClickAction([this](brls::View*) {
        openSearchIme();
        return true;
    });
    m_searchBox->addGestureRecognizer(new brls::TapGestureRecognizer(m_searchBox, [this]() {
        openSearchIme();
    }));

    m_searchChip = new brls::Label();
    m_searchChip->setFontSize(14.0f);
    m_searchChip->setTextColor(nvgRGB(148, 163, 184));
    m_searchChip->setText("搜索游戏 (按 Y)");
    m_searchBox->addView(m_searchChip);
    leftGroup->addView(m_searchBox);

    header->addView(leftGroup);

    m_accountLabel = new brls::Label();
    m_accountLabel->setFontSize(15.0f);
    m_accountLabel->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    m_accountLabel->setTextColor(nvgRGB(203, 213, 225));
    header->addView(m_accountLabel);

    m_root->addView(header);

    // Category Strip
    m_categoryStrip = new brls::Box(brls::Axis::ROW);
    m_categoryStrip->setHeight(48.0f);
    m_categoryStrip->setBackgroundColor(nvgRGB(18, 25, 38));
    m_categoryStrip->setPadding(7.0f, 24.0f, 7.0f, 24.0f);
    m_categoryStrip->setAlignItems(brls::AlignItems::CENTER);
    m_root->addView(m_categoryStrip);

    // Scrolling Game Grid
    m_scrollFrame = new brls::ScrollingFrame();
    m_scrollFrame->setGrow(1.0f);
    m_scrollFrame->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    m_root->addView(m_scrollFrame);

    // Bottom Hints & Game Counter
    auto* footer = new brls::Box(brls::Axis::ROW);
    footer->setHeight(40.0f);
    footer->setBackgroundColor(nvgRGB(18, 24, 36));
    footer->setPadding(0.0f, 24.0f, 0.0f, 24.0f);
    footer->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    footer->setAlignItems(brls::AlignItems::CENTER);

    auto* hintLabel = new brls::Label();
    hintLabel->setText(
        "A 详情/启动  Y 搜索  X 登录  L/R 分类  ZL/ZR 翻页  + 设置  - 刷新");
    hintLabel->setFontSize(14.0f);
    hintLabel->setTextColor(nvgRGB(148, 163, 184));
    footer->addView(hintLabel);

    m_countLabel = new brls::Label();
    m_countLabel->setFontSize(14.0f);
    m_countLabel->setTextColor(nvgRGB(125, 211, 252));
    footer->addView(m_countLabel);

    m_root->addView(footer);

    // Global Lobby Controller Actions
    m_root->registerAction(
        "搜索",
        brls::BUTTON_Y,
        [this](brls::View*) {
            openSearchIme();
            return true;
        },
        true);

    m_root->registerAction(
        "账号/登录",
        brls::BUTTON_X,
        [this](brls::View*) {
            if (m_cfg.cred.sessionKey.empty()) {
                openQrLoginModal();
            } else {
                openAccountModal();
            }
            return true;
        },
        true);

    m_root->registerAction(
        "上一分类",
        brls::BUTTON_LB,
        [this](brls::View*) {
            cycleCategory(-1);
            return true;
        },
        true);

    m_root->registerAction(
        "下一分类",
        brls::BUTTON_RB,
        [this](brls::View*) {
            cycleCategory(1);
            return true;
        },
        true);

    m_root->registerAction(
        "上一页",
        brls::BUTTON_LT,
        [this](brls::View*) {
            changePage(-1);
            return true;
        },
        true);

    m_root->registerAction(
        "下一页",
        brls::BUTTON_RT,
        [this](brls::View*) {
            changePage(1);
            return true;
        },
        true);

    m_root->registerAction(
        "设置",
        brls::BUTTON_START,
        [this](brls::View*) {
            openSettingsModal();
            return true;
        },
        true);

    m_root->registerAction(
        "刷新/清搜",
        brls::BUTTON_BACK,
        [this](brls::View*) {
            if (!m_searchQuery.empty()) {
                m_searchQuery.clear();
                m_currentPage = 0;
                updateHeaderUi();
                rebuildCategoryStrip();
                rebuildGameGrid();
            } else {
                refreshOnlineDataAsync(true);
            }
            return true;
        },
        true);

    return m_root;
}

void LevelUpLobbyActivity::onContentAvailable() {
    startCoverWorker();
    updateHeaderUi();
    rebuildCategoryStrip();
    rebuildGameGrid();

    if (!m_cfg.cred.sessionKey.empty()) {
        refreshOnlineDataAsync(false);
    }
}

void LevelUpLobbyActivity::onResume() {
    m_recent = m_store.loadRecent(m_cfg.cred.userId);
    updateHeaderUi();
    if (m_selectedCategory == "__RECENT__" || m_needsGridRebuild) {
        m_needsGridRebuild = false;
        std::shared_ptr<std::atomic<bool>> alive = m_alive;
        brls::sync([this, alive]() {
            if (!alive->load()) return;
            rebuildCategoryStrip();
            rebuildGameGrid();
        });
    }
}

void LevelUpLobbyActivity::startCoverWorker() {
    if (m_coverThread.joinable()) {
        return;
    }
    {
        std::lock_guard lock(m_coverMu);
        m_coverStop = false;
    }
    m_coverThread = std::thread([this]() { coverWorkerLoop(); });
}

void LevelUpLobbyActivity::stopCoverWorker() {
    {
        std::lock_guard lock(m_coverMu);
        m_coverStop = true;
        m_coverQueue.clear();
    }
    m_coverCv.notify_all();
    if (m_coverThread.joinable()) {
        m_coverThread.join();
    }
}

void LevelUpLobbyActivity::coverWorkerLoop() {
    while (true) {
        CoverTask task;
        {
            std::unique_lock lock(m_coverMu);
            m_coverCv.wait(lock, [this]() { return m_coverStop || !m_coverQueue.empty(); });
            if (m_coverStop) {
                return;
            }
            task = std::move(m_coverQueue.front());
            m_coverQueue.pop_front();
        }

        if (!m_alive->load() || task.generation != m_gridGeneration.load()) {
            continue;
        }
        if (task.gameId.empty() && task.url.empty()) {
            continue;
        }

        std::vector<uint8_t> bytes;
        bool ok = m_store.loadCoverBytes(task.gameId, bytes);
        if (!ok || bytes.size() <= 16) {
            bytes.clear();
            if (!task.url.empty()) {
                std::string err;
                if (api::Client::downloadBinary(task.url, bytes, &err, 2 * 1024 * 1024) &&
                    bytes.size() > 16) {
                    m_store.saveCoverBytes(task.gameId, bytes);
                    ok = true;
                }
            }
        }

        if (!ok || bytes.size() <= 16) {
            continue;
        }

        std::vector<uint8_t> rgba;
        int imgW = 0;
        int imgH = 0;
        if (!decodeDownscaleToRgba(bytes.data(), bytes.size(), 320, 180, rgba, imgW, imgH)) {
            continue;
        }

        std::shared_ptr<std::atomic<bool>> alive = m_alive;
        brls::sync([this,
                    alive,
                    gen = task.generation,
                    img = task.target,
                    imgW,
                    imgH,
                    rgbaData = std::move(rgba)]() {
            if (!alive->load() || m_gridGeneration.load() != gen || !img) {
                return;
            }
            applyRgbaTexture(img, rgbaData, imgW, imgH);
        });
    }
}

void LevelUpLobbyActivity::enqueueCover(const GameEntry& game, uint64_t gen, brls::Image* target) {
    if (!target || (game.gameId.empty() && game.coverUrl.empty())) {
        return;
    }
    {
        std::lock_guard lock(m_coverMu);
        if (m_coverStop) {
            return;
        }
        m_coverQueue.push_back(CoverTask{game.gameId, game.coverUrl, gen, target});
    }
    m_coverCv.notify_one();
}

void LevelUpLobbyActivity::updateHeaderUi() {
    if (m_searchChip) {
        if (m_searchQuery.empty()) {
            m_searchChip->setText("搜索游戏 (按 Y)");
            m_searchChip->setTextColor(nvgRGB(148, 163, 184));
            if (m_searchBox) {
                m_searchBox->setBackgroundColor(nvgRGB(30, 41, 59));
            }
        } else {
            m_searchChip->setText("搜索: \"" + m_searchQuery + "\" (Y修改 / -清除)");
            m_searchChip->setTextColor(nvgRGB(251, 191, 36));
            if (m_searchBox) {
                m_searchBox->setBackgroundColor(nvgRGB(51, 65, 85));
            }
        }
    }

    if (m_accountLabel) {
        if (m_cfg.cred.sessionKey.empty()) {
            m_accountLabel->setText("未登录 · 按 X 扫码登录");
            m_accountLabel->setTextColor(nvgRGB(251, 191, 36));
        } else if (m_hasUserInfo) {
            char buf[220];
            const std::string vip =
                m_userInfo.vipExpireTime.empty() ? "普通" : ("VIP至 " + m_userInfo.vipExpireTime.substr(0, 10));
            const std::string nick =
                !m_userInfo.nickName.empty() ? m_userInfo.nickName : std::to_string(m_userInfo.userId);
            std::snprintf(buf,
                          sizeof(buf),
                          "%s · %s · HP %d分 / MP %d分",
                          nick.c_str(),
                          vip.c_str(),
                          m_userInfo.hpMinutes(),
                          m_userInfo.mpMinutes());
            m_accountLabel->setText(buf);
            m_accountLabel->setTextColor(nvgRGB(134, 239, 172));
        } else {
            const std::string who = !m_cfg.cred.nickName.empty()
                                        ? m_cfg.cred.nickName
                                        : ("UID " + std::to_string(m_cfg.cred.userId));
            m_accountLabel->setText("已登录: " + who);
            m_accountLabel->setTextColor(nvgRGB(134, 239, 172));
        }
    }
}

void LevelUpLobbyActivity::rebuildCategoryStrip() {
    if (!m_categoryStrip) {
        return;
    }
    m_categoryStrip->clearViews();

    struct PillDef {
        std::string id;
        std::string label;
    };
    std::vector<PillDef> pills;
    pills.push_back({"__ALL__", "全部"});
    pills.push_back({"__RECENT__", "最近"});
    for (const auto& cat : m_library.categories) {
        if (!cat.name.empty()) {
            pills.push_back({cat.name, cat.name});
        }
    }

    for (const auto& p : pills) {
        const bool active = (m_selectedCategory == p.id) && m_searchQuery.empty();
        auto* pill = new brls::Box(brls::Axis::ROW);
        pill->setPadding(4.0f, 10.0f, 4.0f, 10.0f);
        pill->setMarginRight(6.0f);
        pill->setCornerRadius(14.0f);
        pill->setBackgroundColor(active ? nvgRGB(2, 132, 199) : nvgRGB(36, 48, 72));

        auto* lbl = new brls::Label();
        lbl->setText(p.label);
        lbl->setFontSize(14.0f);
        lbl->setTextColor(active ? nvgRGB(255, 255, 255) : nvgRGB(203, 213, 225));
        pill->addView(lbl);

        const std::string catId = p.id;
        pill->addGestureRecognizer(new brls::TapGestureRecognizer(pill, [this, catId]() {
            m_selectedCategory = catId;
            m_searchQuery.clear();
            m_currentPage = 0;
            updateHeaderUi();
            rebuildCategoryStrip();
            rebuildGameGrid();
        }));

        m_categoryStrip->addView(pill);
    }
}

std::vector<GameEntry> LevelUpLobbyActivity::currentFilteredGames() const {
    if (!m_searchQuery.empty()) {
        return matchGames(m_library.games, m_searchQuery);
    }

    if (m_selectedCategory == "__ALL__") {
        return m_library.games;
    }

    if (m_selectedCategory == "__RECENT__") {
        std::unordered_map<std::string, const GameEntry*> byId;
        byId.reserve(m_library.games.size());
        for (const auto& g : m_library.games) {
            byId[g.gameId] = &g;
        }
        std::vector<GameEntry> out;
        for (const auto& r : m_recent) {
            auto it = byId.find(r.gameId);
            if (it != byId.end()) {
                out.push_back(*it->second);
            }
        }
        return out;
    }

    std::vector<GameEntry> out;
    for (const auto& g : m_library.games) {
        if (std::find(g.categories.begin(), g.categories.end(), m_selectedCategory) !=
            g.categories.end()) {
            out.push_back(g);
        }
    }
    return out;
}

void LevelUpLobbyActivity::rebuildGameGrid() {
    if (!m_scrollFrame) {
        return;
    }

    const uint64_t gen = m_gridGeneration.fetch_add(1) + 1;
    {
        std::lock_guard lock(m_coverMu);
        m_coverQueue.clear();
    }

    const std::vector<GameEntry> games = currentFilteredGames();
    m_totalPages = std::max<size_t>(1, (games.size() + kGamesPerPage - 1) / kGamesPerPage);
    if (m_currentPage >= m_totalPages) {
        m_currentPage = m_totalPages - 1;
    }
    const size_t startIdx = m_currentPage * kGamesPerPage;
    const size_t endIdx = std::min(games.size(), startIdx + kGamesPerPage);

    if (m_countLabel) {
        if (m_totalPages > 1) {
            m_countLabel->setText("第 " + std::to_string(m_currentPage + 1) + "/" +
                                  std::to_string(m_totalPages) + " 页 · 共 " +
                                  std::to_string(games.size()) + " 款");
        } else {
            m_countLabel->setText("共 " + std::to_string(games.size()) + " 款游戏");
        }
    }

    logInfo("[Lobby] rebuildGameGrid: total=" + std::to_string(games.size()) +
            " page=" + std::to_string(m_currentPage + 1) + "/" + std::to_string(m_totalPages) +
            " showing=" + std::to_string(endIdx > startIdx ? (endIdx - startIdx) : 0));

    auto* gridBox = new CulledGridBox();
    gridBox->setHeight(brls::View::AUTO);
    gridBox->setPadding(12.0f, 16.0f, 20.0f, 16.0f);
    gridBox->setAlignItems(brls::AlignItems::STRETCH);

    brls::View* firstCard = nullptr;

    if (games.empty()) {
        auto* emptyBox = new brls::Box(brls::Axis::COLUMN);
        emptyBox->setHeight(360.0f);
        emptyBox->setJustifyContent(brls::JustifyContent::CENTER);
        emptyBox->setAlignItems(brls::AlignItems::CENTER);

        auto* emptyLabel = new brls::Label();
        if (!m_searchQuery.empty()) {
            emptyLabel->setText("未找到与 \"" + m_searchQuery + "\" 匹配的游戏（按 - 键清除搜索）");
        } else if (m_selectedCategory == "__RECENT__") {
            emptyLabel->setText("暂无最近游玩记录，请先启动一款游戏！");
        } else {
            emptyLabel->setText("当前分类暂无游戏，可按 - 键刷新在线目录");
        }
        emptyLabel->setFontSize(20.0f);
        emptyLabel->setTextColor(nvgRGB(148, 163, 184));
        emptyBox->addView(emptyLabel);

        if (!m_searchQuery.empty()) {
            auto* btnRow = new brls::Box(brls::Axis::ROW);
            btnRow->setMarginTop(18.0f);
            btnRow->setJustifyContent(brls::JustifyContent::CENTER);

            auto* retrySearchBtn = new brls::Button();
            retrySearchBtn->setStyle(&brls::BUTTONSTYLE_PRIMARY);
            retrySearchBtn->setText("重新输入关键词 (Y)");
            retrySearchBtn->setHeight(46.0f);
            retrySearchBtn->setMinWidth(180.0f);
            retrySearchBtn->setMarginRight(12.0f);
            retrySearchBtn->registerClickAction([this](brls::View*) {
                openSearchIme();
                return true;
            });
            btnRow->addView(retrySearchBtn);

            auto* clearSearchBtn = new brls::Button();
            clearSearchBtn->setStyle(&brls::BUTTONSTYLE_DEFAULT);
            clearSearchBtn->setText("清除搜索 (-)");
            clearSearchBtn->setHeight(46.0f);
            clearSearchBtn->setMinWidth(150.0f);
            clearSearchBtn->registerClickAction([this](brls::View*) {
                m_searchQuery.clear();
                m_currentPage = 0;
                updateHeaderUi();
                rebuildCategoryStrip();
                rebuildGameGrid();
                return true;
            });
            btnRow->addView(clearSearchBtn);

            emptyBox->addView(btnRow);
            firstCard = retrySearchBtn;
        }

        gridBox->addView(emptyBox);
    } else {
        constexpr size_t kCols = 4;
        for (size_t i = startIdx; i < endIdx; i += kCols) {
            auto* row = new brls::Box(brls::Axis::ROW);
            row->setHeight(brls::View::AUTO);
            row->setJustifyContent(brls::JustifyContent::FLEX_START);
            row->setMarginBottom(8.0f);

            for (size_t c = 0; c < kCols && (i + c) < endIdx; ++c) {
                const GameEntry& game = games[i + c];

                auto* card = new brls::Box(brls::Axis::COLUMN);
                card->setWidth(290.0f);
                card->setHeight(218.0f);
                card->setMargins(6.0f, 8.0f, 6.0f, 8.0f);
                card->setCornerRadius(12.0f);
                card->setHighlightCornerRadius(12.0f);
                card->setBackgroundColor(nvgRGB(27, 37, 56));
                card->setFocusable(true);

                auto* imgFrame = new brls::Box(brls::Axis::COLUMN);
                imgFrame->setWidth(290.0f);
                imgFrame->setHeight(152.0f);
                imgFrame->setBackgroundColor(nvgRGB(19, 26, 40));

                auto* img = new brls::Image();
                img->setDimensions(290.0f, 152.0f);
                img->setScalingType(brls::ImageScalingType::FILL);
                img->setCornerRadius(10.0f);
                imgFrame->addView(img);
                card->addView(imgFrame);

                auto* info = new brls::Box(brls::Axis::COLUMN);
                info->setPadding(8.0f, 12.0f, 8.0f, 12.0f);
                info->setJustifyContent(brls::JustifyContent::CENTER);
                info->setGrow(1.0f);

                auto* title = new brls::Label();
                title->setText(game.name.empty() ? game.fullName : game.name);
                title->setFontSize(17.0f);
                title->setSingleLine(true);
                title->setTextColor(nvgRGB(248, 250, 252));
                info->addView(title);

                auto* tag = new brls::Label();
                tag->setText(joinGameCategories(game.categories));
                tag->setFontSize(13.0f);
                tag->setSingleLine(true);
                tag->setTextColor(nvgRGB(148, 163, 184));
                info->addView(tag);

                card->addView(info);

                card->registerClickAction([this, game](brls::View*) {
                    openGameDetailModal(game);
                    return true;
                });
                card->addGestureRecognizer(new brls::TapGestureRecognizer(card));

                if (!firstCard) {
                    firstCard = card;
                }
                row->addView(card);

                enqueueCover(game, gen, img);
            }
            gridBox->addView(row);
        }

        if (m_totalPages > 1) {
            auto* pagerRow = new brls::Box(brls::Axis::ROW);
            pagerRow->setHeight(58.0f);
            pagerRow->setMarginTop(8.0f);
            pagerRow->setJustifyContent(brls::JustifyContent::CENTER);
            pagerRow->setAlignItems(brls::AlignItems::CENTER);

            const bool hasPrev = m_currentPage > 0;
            auto* prevBtn = new brls::Button();
            prevBtn->setStyle(hasPrev ? &brls::BUTTONSTYLE_PRIMARY : &brls::BUTTONSTYLE_DEFAULT);
            prevBtn->setText("上一页 (ZL)");
            if (!hasPrev) {
                prevBtn->setTextColor(nvgRGB(100, 116, 139));
            }
            prevBtn->setHeight(42.0f);
            prevBtn->setMinWidth(160.0f);
            prevBtn->setMarginRight(20.0f);
            prevBtn->registerClickAction([this](brls::View*) {
                changePage(-1);
                return true;
            });
            pagerRow->addView(prevBtn);

            auto* pageLbl = new brls::Label();
            pageLbl->setText("第 " + std::to_string(m_currentPage + 1) + " / " +
                             std::to_string(m_totalPages) + " 页");
            pageLbl->setFontSize(16.0f);
            pageLbl->setTextColor(nvgRGB(203, 213, 225));
            pageLbl->setMarginRight(20.0f);
            pagerRow->addView(pageLbl);

            const bool hasNext = (m_currentPage + 1) < m_totalPages;
            auto* nextBtn = new brls::Button();
            nextBtn->setStyle(hasNext ? &brls::BUTTONSTYLE_PRIMARY : &brls::BUTTONSTYLE_DEFAULT);
            nextBtn->setText("下一页 (ZR)");
            if (!hasNext) {
                nextBtn->setTextColor(nvgRGB(100, 116, 139));
            }
            nextBtn->setHeight(42.0f);
            nextBtn->setMinWidth(160.0f);
            nextBtn->registerClickAction([this](brls::View*) {
                changePage(1);
                return true;
            });
            pagerRow->addView(nextBtn);

            gridBox->addView(pagerRow);
        }
    }

    m_scrollFrame->setContentView(gridBox);
    m_scrollFrame->setContentOffsetY(0.0f, false);

    const auto& stack = brls::Application::getActivitiesStack();
    if (!stack.empty() && stack.back() == this) {
        brls::Application::giveFocus(firstCard ? firstCard : static_cast<brls::View*>(m_scrollFrame));
    }
}

void LevelUpLobbyActivity::openSearchIme() {
    auto* ime = brls::Application::getPlatform()->getImeManager();
    if (!ime) {
        return;
    }
    ime->openForText(
        [this](std::string text) {
            m_searchQuery = text;
            m_currentPage = 0;
            updateHeaderUi();
            rebuildCategoryStrip();
            rebuildGameGrid();
        },
        "搜索 LevelUp 云游戏",
        "支持中文名 / 英文名 / 拼音或全名关键字（留空确定可重置）",
        48,
        m_searchQuery);
}

void LevelUpLobbyActivity::cycleCategory(int delta) {
    std::vector<std::string> ids;
    ids.push_back("__ALL__");
    ids.push_back("__RECENT__");
    for (const auto& c : m_library.categories) {
        if (!c.name.empty()) {
            ids.push_back(c.name);
        }
    }
    if (ids.empty()) {
        return;
    }
    int cur = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == m_selectedCategory) {
            cur = static_cast<int>(i);
            break;
        }
    }
    const int n = static_cast<int>(ids.size());
    const int next = ((cur + delta) % n + n) % n;
    m_selectedCategory = ids[next];
    m_searchQuery.clear();
    m_currentPage = 0;
    updateHeaderUi();
    rebuildCategoryStrip();
    rebuildGameGrid();
}

void LevelUpLobbyActivity::changePage(int delta) {
    if (m_totalPages <= 1) {
        return;
    }
    const int next = std::clamp<int>(
        static_cast<int>(m_currentPage) + delta, 0, static_cast<int>(m_totalPages) - 1);
    if (static_cast<size_t>(next) != m_currentPage) {
        m_currentPage = static_cast<size_t>(next);
        rebuildGameGrid();
    }
}

void LevelUpLobbyActivity::openGameDetailModal(const GameEntry& game) {
    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(nvgRGBA(6, 10, 18, 200));

    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setWidth(660.0f);
    panel->setPadding(24.0f);
    panel->setCornerRadius(16.0f);
    panel->setBackgroundColor(nvgRGB(24, 32, 48));
    panel->setAlignItems(brls::AlignItems::STRETCH);

    auto* topRow = new brls::Box(brls::Axis::ROW);
    topRow->setMarginBottom(16.0f);

    auto* cover = new brls::Image();
    cover->setDimensions(260.0f, 146.0f);
    cover->setScalingType(brls::ImageScalingType::FILL);
    cover->setCornerRadius(10.0f);
    cover->setMarginRight(18.0f);
    std::vector<uint8_t> coverBytes;
    if (m_store.loadCoverBytes(game.gameId, coverBytes) && coverBytes.size() > 16) {
        std::vector<uint8_t> rgba;
        int imgW = 0;
        int imgH = 0;
        if (decodeDownscaleToRgba(
                coverBytes.data(), coverBytes.size(), 320, 180, rgba, imgW, imgH)) {
            applyRgbaTexture(cover, rgba, imgW, imgH);
        }
    }
    topRow->addView(cover);

    auto* meta = new brls::Box(brls::Axis::COLUMN);
    meta->setGrow(1.0f);

    auto* title = new brls::Label();
    title->setText(game.name.empty() ? game.fullName : game.name);
    title->setFontSize(24.0f);
    title->setTextColor(nvgRGB(248, 250, 252));
    title->setMarginBottom(6.0f);
    meta->addView(title);

    if (!game.fullName.empty() && game.fullName != game.name) {
        auto* sub = new brls::Label();
        sub->setText(game.fullName);
        sub->setFontSize(14.0f);
        sub->setTextColor(nvgRGB(148, 163, 184));
        sub->setMarginBottom(6.0f);
        meta->addView(sub);
    }

    auto* catLbl = new brls::Label();
    catLbl->setText("分类: " + joinGameCategories(game.categories) + "  |  ID: " + game.gameId);
    catLbl->setFontSize(14.0f);
    catLbl->setTextColor(nvgRGB(125, 211, 252));
    meta->addView(catLbl);

    topRow->addView(meta);
    panel->addView(topRow);

    if (!game.description.empty()) {
        auto* desc = new brls::Label();
        desc->setText(truncateUtf8(game.description, 260));
        desc->setFontSize(15.0f);
        desc->setSingleLine(false);
        desc->setTextColor(nvgRGB(203, 213, 225));
        desc->setMarginBottom(14.0f);
        panel->addView(desc);
    }

    char cfgBuf[160];
    std::snprintf(cfgBuf,
                  sizeof(cfgBuf),
                  "输出: %s · 画质档位: %d (%s) · HD震动: %s (%d%%)",
                  detectDisplayBadge().c_str(),
                  m_cfg.videoLevel,
                  m_cfg.vqFirst ? "VQ_FIRST" : "默认",
                  m_cfg.rumbleEnabled ? "开" : "关",
                  m_cfg.rumbleStrength);
    auto* cfgLbl = new brls::Label();
    cfgLbl->setText(cfgBuf);
    cfgLbl->setFontSize(14.0f);
    cfgLbl->setTextColor(nvgRGB(148, 163, 184));
    cfgLbl->setMarginBottom(18.0f);
    panel->addView(cfgLbl);

    auto closed = std::make_shared<std::atomic<bool>>(false);
    auto closeModal = [closed](std::function<void()> afterClose = nullptr) {
        if (!closed->exchange(true)) {
            brls::sync([after = std::move(afterClose)]() {
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
                if (after) {
                    after();
                }
            });
        }
    };

    auto* btnRow = new brls::Box(brls::Axis::ROW);
    btnRow->setJustifyContent(brls::JustifyContent::FLEX_END);

    auto* backBtn = new brls::Button();
    backBtn->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    backBtn->setText("返回");
    backBtn->setHeight(48.0f);
    backBtn->setMinWidth(130.0f);
    backBtn->setMarginRight(12.0f);
    backBtn->registerClickAction([closeModal](brls::View*) {
        closeModal();
        return true;
    });
    btnRow->addView(backBtn);

    auto* startBtn = new brls::Button();
    startBtn->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    startBtn->setText(m_cfg.cred.sessionKey.empty() ? "扫码登录并启动" : "启动云游戏");
    startBtn->setHeight(48.0f);
    startBtn->setMinWidth(200.0f);
    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    startBtn->registerClickAction([this, alive, game, closeModal](brls::View*) {
        closeModal([this, alive, game]() {
            if (!alive->load()) {
                return;
            }
            if (m_cfg.cred.sessionKey.empty()) {
                m_pendingLaunchGame = game;
                brls::Application::notify("请先扫码登录 LevelUp 账号");
                openQrLoginModal();
            } else {
                launchGame(game);
            }
        });
        return true;
    });
    btnRow->addView(startBtn);

    panel->addView(btnRow);
    backdrop->addView(panel);

    backdrop->registerAction("关闭", brls::BUTTON_B, [closeModal](brls::View*) {
        closeModal();
        return true;
    });

    brls::Application::pushActivity(new TranslucentModalActivity(backdrop),
                                    brls::TransitionAnimation::NONE);
    brls::Application::giveFocus(startBtn);
}

void LevelUpLobbyActivity::launchGame(const GameEntry& game) {
    m_recent = m_store.recordRecent(m_cfg.cred.userId, game.gameId);
    auto* streamView = new LevelUpStreamView(
        game,
        m_cfg,
        m_store,
        [this](const AppConfig& updated) { m_cfg = updated; });
    brls::Application::pushActivity(new brls::Activity(streamView),
                                    brls::TransitionAnimation::FADE);
}

void LevelUpLobbyActivity::openQrLoginModal() {
    m_qrCancel.store(true);
    if (m_qrThread.joinable()) {
        m_qrThread.join();
    }
    m_qrCancel.store(false);

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(nvgRGBA(6, 10, 18, 200));

    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setWidth(460.0f);
    panel->setPadding(24.0f);
    panel->setCornerRadius(16.0f);
    panel->setBackgroundColor(nvgRGB(24, 32, 48));
    panel->setAlignItems(brls::AlignItems::CENTER);

    auto* title = new brls::Label();
    title->setText("扫码登录 LevelUp 账号");
    title->setFontSize(22.0f);
    title->setTextColor(nvgRGB(248, 250, 252));
    title->setMarginBottom(16.0f);
    panel->addView(title);

    auto* qrFrame = new brls::Box(brls::Axis::COLUMN);
    qrFrame->setDimensions(268.0f, 268.0f);
    qrFrame->setPadding(8.0f);
    qrFrame->setCornerRadius(10.0f);
    qrFrame->setBackgroundColor(nvgRGB(255, 255, 255));
    qrFrame->setJustifyContent(brls::JustifyContent::CENTER);
    qrFrame->setAlignItems(brls::AlignItems::CENTER);
    qrFrame->setMarginBottom(14.0f);

    auto* qrImg = new brls::Image();
    qrImg->setDimensions(252.0f, 252.0f);
    qrImg->setScalingType(brls::ImageScalingType::FIT);
    qrFrame->addView(qrImg);
    panel->addView(qrFrame);

    auto* statusLbl = new brls::Label();
    statusLbl->setText("正在获取登录二维码...");
    statusLbl->setFontSize(15.0f);
    statusLbl->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    statusLbl->setSingleLine(false);
    statusLbl->setTextColor(nvgRGB(148, 163, 184));
    statusLbl->setMarginBottom(16.0f);
    panel->addView(statusLbl);

    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    auto modalOpen = std::make_shared<std::atomic<bool>>(true);
    auto closeQr = [this, alive, modalOpen]() {
        m_qrCancel.store(true);
        if (modalOpen->exchange(false)) {
            brls::sync([this, alive]() {
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
                if (alive->load()) {
                    m_pendingLaunchGame.reset();
                }
            });
        }
    };

    auto* cancelBtn = new brls::Button();
    cancelBtn->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    cancelBtn->setText("取消");
    cancelBtn->setHeight(46.0f);
    cancelBtn->setMinWidth(150.0f);
    cancelBtn->registerClickAction([closeQr](brls::View*) {
        closeQr();
        return true;
    });
    panel->addView(cancelBtn);

    backdrop->addView(panel);
    backdrop->registerAction("取消", brls::BUTTON_B, [closeQr](brls::View*) {
        closeQr();
        return true;
    });

    brls::Application::pushActivity(new TranslucentModalActivity(backdrop),
                                    brls::TransitionAnimation::NONE);
    brls::Application::giveFocus(cancelBtn);

    std::string deviceId = m_cfg.deviceId;

    m_qrThread = std::thread([this, alive, modalOpen, qrImg, statusLbl, deviceId]() {
        api::Client client(deviceId, {});
        api::QRCodeResult qr;
        std::string err;
        if (!client.getQRCode(qr, &err)) {
            brls::sync([alive, modalOpen, statusLbl, err]() {
                if (!alive->load() || !modalOpen->load()) return;
                statusLbl->setText("获取二维码失败: " + err);
                statusLbl->setTextColor(nvgRGB(248, 113, 113));
            });
            return;
        }

        std::vector<uint8_t> jpeg = base64Decode(qr.buffer);
        std::vector<uint8_t> rgba;
        int qrW = 0;
        int qrH = 0;
        const bool decodedQr =
            !jpeg.empty() && decodeDownscaleToRgba(jpeg.data(), jpeg.size(), 360, 360, rgba, qrW, qrH);
        brls::sync([alive,
                    modalOpen,
                    qrImg,
                    statusLbl,
                    decodedQr,
                    qrW,
                    qrH,
                    imgData = std::move(rgba)]() {
            if (!alive->load() || !modalOpen->load()) return;
            if (decodedQr) {
                applyRgbaTexture(qrImg, imgData, qrW, qrH);
            }
            statusLbl->setText("请使用微信 / LevelUp App 扫码登录");
            statusLbl->setTextColor(nvgRGB(125, 211, 252));
        });

        for (int attempt = 0; attempt < 90; ++attempt) {
            for (int step = 0; step < 20; ++step) {
                if (!alive->load() || m_qrCancel.load() || !modalOpen->load()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            api::LoginStateResult st;
            std::string pollErr;
            if (!client.pollQRCodeState(qr.codeId, st, &pollErr)) {
                continue;
            }

            if (st.state == 1) {
                brls::sync([alive, modalOpen, statusLbl]() {
                    if (!alive->load() || !modalOpen->load()) return;
                    statusLbl->setText("已扫描，请在手机上点击确认登录...");
                    statusLbl->setTextColor(nvgRGB(251, 191, 36));
                });
            } else if (st.state == 2 && !st.sessionKey.empty()) {
                api::Credentials cred;
                cred.sessionKey = st.sessionKey;
                cred.ciKey = st.ciKey;
                cred.secretKey = st.secretKey;

                api::Client authed(deviceId, cred);
                api::UserInfo info;
                std::string uerr;
                const bool gotUser = authed.getUserInfo(info, &uerr);
                if (gotUser && info.userId > 0) {
                    cred.userId = info.userId;
                    cred.nickName = info.nickName;
                    cred.mobile = info.mobile;
                }

                brls::sync([this, alive, modalOpen, cred, info, gotUser]() {
                    if (!alive->load()) return;
                    if (modalOpen->exchange(false)) {
                        brls::Application::popActivity(brls::TransitionAnimation::NONE);
                    }
                    m_cfg.cred = cred;
                    if (gotUser && info.userId > 0) {
                        m_userInfo = info;
                        m_hasUserInfo = true;
                    }
                    m_store.saveConfig(m_cfg);
                    m_recent = m_store.loadRecent(m_cfg.cred.userId);
                    updateHeaderUi();
                    rebuildGameGrid();

                    const std::string displayWho =
                        !m_cfg.cred.nickName.empty() ? m_cfg.cred.nickName
                                                     : ("UID " + std::to_string(m_cfg.cred.userId));
                    brls::Application::notify("登录成功：" + displayWho);

                    if (m_pendingLaunchGame.has_value()) {
                        GameEntry pending = *m_pendingLaunchGame;
                        m_pendingLaunchGame.reset();
                        launchGame(pending);
                    } else {
                        refreshOnlineDataAsync(false);
                    }
                });
                return;
            } else if (st.state >= 3) {
                brls::sync([alive, modalOpen, statusLbl]() {
                    if (!alive->load() || !modalOpen->load()) return;
                    statusLbl->setText("二维码已过期，请点击取消后重试");
                    statusLbl->setTextColor(nvgRGB(248, 113, 113));
                });
                return;
            }
        }

        brls::sync([alive, modalOpen, statusLbl]() {
            if (!alive->load() || !modalOpen->load()) return;
            statusLbl->setText("扫码超时，请重试");
            statusLbl->setTextColor(nvgRGB(248, 113, 113));
        });
    });
}

void LevelUpLobbyActivity::openAccountModal() {
    auto theme = brls::Application::getTheme();

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(nvgRGBA(6, 10, 18, 195));

    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setWidth(520.0f);
    panel->setPadding(24.0f);
    panel->setCornerRadius(16.0f);
    panel->setBackgroundColor(theme["brls/background"]);
    panel->setAlignItems(brls::AlignItems::STRETCH);

    auto* title = new brls::Label();
    title->setText("LevelUp 账号信息");
    title->setFontSize(24.0f);
    title->setTextColor(theme["brls/text"]);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setMarginBottom(14.0f);
    panel->addView(title);

    std::string summary;
    const std::string nick = !m_cfg.cred.nickName.empty() ? m_cfg.cred.nickName : "未知";
    summary += "昵称: " + nick + "\n";
    summary += "UID: " + std::to_string(m_cfg.cred.userId) + "\n";
    if (m_hasUserInfo) {
        summary += "VIP 到期: " +
                   (m_userInfo.vipExpireTime.empty() ? std::string("无/未知") : m_userInfo.vipExpireTime) +
                   "\n";
        summary += "HP 时长: " + std::to_string(m_userInfo.hpMinutes()) + " 分钟  |  MP 时长: " +
                   std::to_string(m_userInfo.mpMinutes()) + " 分钟";
    } else {
        summary += "点击下方“刷新账号信息”获取最新 VIP / HP / MP 时长。";
    }

    auto* infoLbl = new brls::Label();
    infoLbl->setText(summary);
    infoLbl->setFontSize(17.0f);
    infoLbl->setSingleLine(false);
    infoLbl->setTextColor(theme["brls/text"]);
    infoLbl->setMarginBottom(18.0f);
    panel->addView(infoLbl);

    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    auto closed = std::make_shared<std::atomic<bool>>(false);
    auto closeModal = [closed](std::function<void()> afterClose = nullptr) {
        if (!closed->exchange(true)) {
            brls::sync([after = std::move(afterClose)]() {
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
                if (after) {
                    after();
                }
            });
        }
    };

    auto makeBtn = [](const std::string& label, const brls::ButtonStyle* style) {
        auto* btn = new brls::Button();
        btn->setStyle(style);
        btn->setText(label);
        btn->setHeight(48.0f);
        btn->setMargins(5.0f, 0.0f, 5.0f, 0.0f);
        return btn;
    };

    auto* refreshBtn = makeBtn("刷新账号与在线游戏库", &brls::BUTTONSTYLE_PRIMARY);
    refreshBtn->registerClickAction([this, alive, closeModal](brls::View*) {
        closeModal([this, alive]() {
            if (alive->load()) {
                refreshOnlineDataAsync(true);
            }
        });
        return true;
    });
    panel->addView(refreshBtn);

    auto* rescanBtn = makeBtn("重新扫码切换账号", &brls::BUTTONSTYLE_DEFAULT);
    rescanBtn->registerClickAction([this, alive, closeModal](brls::View*) {
        closeModal([this, alive]() {
            if (alive->load()) {
                openQrLoginModal();
            }
        });
        return true;
    });
    panel->addView(rescanBtn);

    auto* logoutBtn = makeBtn("退出当前账号", &brls::BUTTONSTYLE_DEFAULT);
    logoutBtn->setTextColor(nvgRGB(220, 60, 60));
    logoutBtn->registerClickAction([this, alive, closeModal](brls::View*) {
        closeModal([this, alive]() {
            if (!alive->load()) {
                return;
            }
            m_cfg.cred = {};
            m_hasUserInfo = false;
            m_userInfo = {};
            m_store.saveConfig(m_cfg);
            m_recent = m_store.loadRecent(0);
            updateHeaderUi();
            rebuildGameGrid();
            brls::Application::notify("已退出登录");
        });
        return true;
    });
    panel->addView(logoutBtn);

    auto* closeBtn = makeBtn("返回", &brls::BUTTONSTYLE_DEFAULT);
    closeBtn->registerClickAction([closeModal](brls::View*) {
        closeModal();
        return true;
    });
    panel->addView(closeBtn);

    backdrop->addView(panel);
    backdrop->registerAction("返回", brls::BUTTON_B, [closeModal](brls::View*) {
        closeModal();
        return true;
    });

    brls::Application::pushActivity(new TranslucentModalActivity(backdrop),
                                    brls::TransitionAnimation::NONE);
    brls::Application::giveFocus(refreshBtn);
}

void LevelUpLobbyActivity::openSettingsModal() {
    auto theme = brls::Application::getTheme();

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(nvgRGBA(6, 10, 18, 195));

    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setWidth(660.0f);
    panel->setHeight(620.0f);
    panel->setPadding(22.0f);
    panel->setCornerRadius(16.0f);
    panel->setBackgroundColor(theme["brls/background"]);
    panel->setAlignItems(brls::AlignItems::STRETCH);

    auto* title = new brls::Label();
    title->setText("LevelUp-NX 设置");
    title->setFontSize(24.0f);
    title->setTextColor(theme["brls/text"]);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setMarginBottom(12.0f);
    panel->addView(title);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    auto* list = new brls::Box(brls::Axis::COLUMN);
    list->setHeight(brls::View::AUTO);
    list->setAlignItems(brls::AlignItems::STRETCH);

    auto* rumbleCell = new brls::BooleanCell();
    rumbleCell->init("HD 手柄震动", m_cfg.rumbleEnabled, [this](bool on) {
        m_cfg.rumbleEnabled = on;
        m_store.saveConfig(m_cfg);
    });
    list->addView(rumbleCell);

    auto* rumbleStrCell = new brls::SelectorCell();
    rumbleStrCell->init(
        "HD 震动强度",
        {"0%", "25%", "50%", "75%", "100%"},
        percentToFiveStepIndex(m_cfg.rumbleStrength),
        [this](int idx) {
            m_cfg.rumbleStrength = fiveStepIndexToPercent(idx);
            m_store.saveConfig(m_cfg);
        });
    list->addView(rumbleStrCell);

    auto* levelCell = new brls::SelectorCell();
    levelCell->init(
        "云端视频画质档位",
        {"0 - 流畅", "1 - 标清", "2 - 高清", "3 - 超清 (推荐)", "4 - 原画"},
        std::clamp(m_cfg.videoLevel, 0, 4),
        [this](int idx) {
            m_cfg.videoLevel = std::clamp(idx, 0, 4);
            m_store.saveConfig(m_cfg);
        });
    list->addView(levelCell);

    auto* vqCell = new brls::BooleanCell();
    vqCell->init("画质优先模式 (VQ_FIRST)", m_cfg.vqFirst, [this](bool on) {
        m_cfg.vqFirst = on;
        m_store.saveConfig(m_cfg);
    });
    list->addView(vqCell);

    auto* hudCell = new brls::BooleanCell();
    hudCell->init("默认显示性能 HUD", m_cfg.showStatsHud, [this](bool on) {
        m_cfg.showStatsHud = on;
        m_store.saveConfig(m_cfg);
    });
    list->addView(hudCell);

    auto* invertYCell = new brls::BooleanCell();
    invertYCell->init("反转摇杆 Y 轴", m_cfg.invertStickY, [this](bool on) {
        m_cfg.invertStickY = on;
        m_store.saveConfig(m_cfg);
    });
    list->addView(invertYCell);

    auto* volCell = new brls::SelectorCell();
    volCell->init(
        "串流音频音量",
        {"0%", "25%", "50%", "75%", "100%"},
        percentToFiveStepIndex(m_cfg.audioVolume),
        [this](int idx) {
            m_cfg.audioVolume = fiveStepIndexToPercent(idx);
            m_store.saveConfig(m_cfg);
        });
    list->addView(volCell);

    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    auto closed = std::make_shared<std::atomic<bool>>(false);
    auto closeModal = [closed](std::function<void()> afterClose = nullptr) {
        if (!closed->exchange(true)) {
            brls::sync([after = std::move(afterClose)]() {
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
                if (after) {
                    after();
                }
            });
        }
    };

    auto makeActionBtn = [](const std::string& label, const brls::ButtonStyle* style) {
        auto* btn = new brls::Button();
        btn->setStyle(style);
        btn->setText(label);
        btn->setHeight(48.0f);
        btn->setMargins(8.0f, 16.0f, 2.0f, 16.0f);
        return btn;
    };

    auto* syncLibBtn = makeActionBtn("刷新在线游戏库", &brls::BUTTONSTYLE_DEFAULT);
    syncLibBtn->registerClickAction([this, alive, closeModal](brls::View*) {
        closeModal([this, alive]() {
            if (alive->load()) {
                refreshOnlineDataAsync(true);
            }
        });
        return true;
    });
    list->addView(syncLibBtn);

    auto* clearCacheBtn = makeActionBtn("清除本地封面缓存", &brls::BUTTONSTYLE_DEFAULT);
    clearCacheBtn->registerClickAction([this, alive, closeModal](brls::View*) {
        closeModal([this, alive]() {
            if (!alive->load()) {
                return;
            }
            m_store.clearCoversCache();
            brls::Application::notify("已清除本地封面缓存");
            rebuildGameGrid();
        });
        return true;
    });
    list->addView(clearCacheBtn);

#ifdef __SWITCH__
    auto* forwarderBtn = makeActionBtn("安装到 Switch 主界面（桌面图标）", &brls::BUTTONSTYLE_DEFAULT);
    forwarderBtn->registerClickAction([alive, closeModal](brls::View*) {
        closeModal([alive]() {
            if (!alive->load()) {
                return;
            }
            const int rc = makeAppForwarder("LevelUp-NX", "LevelUp-Switch");
            if (rc == 0) {
                logInfo("[Forwarder] installed LevelUp-NX home menu forwarder");
                brls::Application::notify("已安装到 Switch 主界面，请返回桌面查看");
            } else {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "主界面安装失败 (0x%X)，需确认大气层签名补丁/sys-patch", rc);
                logWarn(std::string("[Forwarder] ") + buf);
                brls::Application::notify(buf);
            }
        });
        return true;
    });
    list->addView(forwarderBtn);
#endif

    if (!m_cfg.cred.sessionKey.empty()) {
        auto* logoutBtn = makeActionBtn("退出当前账号", &brls::BUTTONSTYLE_DEFAULT);
        logoutBtn->setTextColor(nvgRGB(220, 60, 60));
        logoutBtn->registerClickAction([this, alive, closeModal](brls::View*) {
            closeModal([this, alive]() {
                if (!alive->load()) {
                    return;
                }
                m_cfg.cred = {};
                m_hasUserInfo = false;
                m_userInfo = {};
                m_store.saveConfig(m_cfg);
                m_recent = m_store.loadRecent(0);
                updateHeaderUi();
                rebuildGameGrid();
                brls::Application::notify("已退出登录");
            });
            return true;
        });
        list->addView(logoutBtn);
    }

    auto* doneBtn = makeActionBtn("完成", &brls::BUTTONSTYLE_PRIMARY);
    doneBtn->setMarginBottom(10.0f);
    doneBtn->registerClickAction([closeModal](brls::View*) {
        closeModal();
        return true;
    });
    list->addView(doneBtn);

    scroll->setContentView(list);
    panel->addView(scroll);
    backdrop->addView(panel);

    backdrop->registerAction("返回", brls::BUTTON_B, [closeModal](brls::View*) {
        closeModal();
        return true;
    });

    brls::Application::pushActivity(new TranslucentModalActivity(backdrop),
                                    brls::TransitionAnimation::NONE);
    brls::Application::giveFocus(rumbleCell);
}

void LevelUpLobbyActivity::refreshOnlineDataAsync(bool notifyResult) {
    if (m_netBusy.exchange(true)) {
        if (notifyResult) {
            brls::Application::notify("正在刷新，请稍候...");
        }
        return;
    }
    if (m_netThread.joinable()) {
        m_netThread.join();
    }

    const std::string deviceId = m_cfg.deviceId;
    const api::Credentials cred = m_cfg.cred;
    const GamesLibrary baseLib = m_library;
    std::shared_ptr<std::atomic<bool>> alive = m_alive;

    m_netThread = std::thread([this, alive, deviceId, cred, baseLib, notifyResult]() {
        api::Client client(deviceId, cred);

        api::UserInfo info;
        bool gotUser = false;
        std::string userErr;
        if (!cred.sessionKey.empty()) {
            gotUser = client.getUserInfo(info, &userErr);
        }

        std::vector<api::GameListItem> allOnline;
        bool gotGames = false;
        std::string listErr;
        for (int page = 1; page <= 5; ++page) {
            api::ListGamesResult pageRes;
            if (!client.listGames(page, 100, pageRes, &listErr)) {
                break;
            }
            gotGames = true;
            if (pageRes.list.empty()) {
                break;
            }
            allOnline.insert(allOnline.end(), pageRes.list.begin(), pageRes.list.end());
            if (static_cast<int>(pageRes.list.size()) < 100) {
                break;
            }
        }

        GamesLibrary merged;
        if (gotGames && !allOnline.empty()) {
            merged = Store::mergeOnlineGames(baseLib, allOnline);
        }

        brls::sync([this,
                    alive,
                    gotUser,
                    info,
                    userErr,
                    gotGames,
                    onlineCount = allOnline.size(),
                    merged = std::move(merged),
                    notifyResult]() {
            m_netBusy.store(false);
            if (!alive->load()) {
                return;
            }

            if (gotUser && info.userId > 0) {
                const bool uidChanged = (m_cfg.cred.userId != info.userId);
                m_userInfo = info;
                m_hasUserInfo = true;
                m_cfg.cred.userId = info.userId;
                if (!info.nickName.empty()) {
                    m_cfg.cred.nickName = info.nickName;
                }
                if (!info.mobile.empty()) {
                    m_cfg.cred.mobile = info.mobile;
                }
                m_store.saveConfig(m_cfg);
                if (uidChanged) {
                    m_recent = m_store.loadRecent(m_cfg.cred.userId);
                }
            }

            if (gotGames && !merged.games.empty()) {
                m_library = std::move(merged);
                m_store.saveLibrary(m_library);
                const auto& stack = brls::Application::getActivitiesStack();
                if (!stack.empty() && stack.back() == this) {
                    rebuildCategoryStrip();
                    rebuildGameGrid();
                } else {
                    m_needsGridRebuild = true;
                }
            }

            updateHeaderUi();
            if (notifyResult) {
                if (gotGames) {
                    brls::Application::notify("已更新在线目录（" + std::to_string(onlineCount) + " 款）");
                } else if (gotUser) {
                    brls::Application::notify("已刷新账号信息");
                } else {
                    brls::Application::notify("刷新失败，请检查网络连接");
                }
            }
        });
    });
}

} // namespace levelup::ui
