#include "levelup/ui/lobby_activity.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/exit_guard.hpp"
#include "levelup/log.hpp"
#include "levelup/rumble_switch.hpp"
#include "levelup/ui/levelup_stream_view.hpp"
#include "levelup/ytcx_client.hpp"

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

    // 大比例缩小时用面积平均（box filter），避免双线性点采样丢失细节/产生摩尔纹；
    // 小比例缩小仍走双线性（更快也更平滑）。
    if (scale < 0.55) {
        const double stepX = static_cast<double>(srcW) / static_cast<double>(dstW);
        const double stepY = static_cast<double>(srcH) / static_cast<double>(dstH);
        for (int dy = 0; dy < dstH; ++dy) {
            const int sy0 = std::clamp(static_cast<int>(dy * stepY), 0, srcH - 1);
            const int sy1 = std::clamp(static_cast<int>(std::ceil((dy + 1) * stepY)), sy0 + 1, srcH);
            for (int dx = 0; dx < dstW; ++dx) {
                const int sx0 = std::clamp(static_cast<int>(dx * stepX), 0, srcW - 1);
                const int sx1 = std::clamp(static_cast<int>(std::ceil((dx + 1) * stepX)), sx0 + 1, srcW);

                uint32_t acc[4] = {0, 0, 0, 0};
                for (int sy = sy0; sy < sy1; ++sy) {
                    const uint8_t* row = pixels + (static_cast<size_t>(sy) * srcW + sx0) * 4;
                    for (int sx = sx0; sx < sx1; ++sx) {
                        for (int c = 0; c < 4; ++c) {
                            acc[c] += row[(sx - sx0) * 4 + c];
                        }
                    }
                }
                const uint32_t n =
                    static_cast<uint32_t>((sx1 - sx0) * (sy1 - sy0));
                uint8_t* dst = outRgba.data() + (static_cast<size_t>(dy) * dstW + dx) * 4;
                for (int c = 0; c < 4; ++c) {
                    dst[c] = static_cast<uint8_t>(acc[c] / (n > 0 ? n : 1));
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

/// 封面纹理的解码上限：按控件在屏上的**实际像素**算，而不是写死 320x180。
///
/// 卡片里的封面控件是 290x152（逻辑像素），Borealis 在底座 1080p 下用
/// `windowScale = 1.5` 把整个 UI 放大绘制，于是实际占 435x228 物理像素。
/// 竖版封面（云游戏封面基本是 3:4）在 contain-fit 下被"高度"卡住，旧代码
/// 320x180 只会解出约 135x180 的纹理，再被 FILL 拉到 435x228 上屏 —— 这就是糊的根因。
///
/// 现在按 宽度 = 控件物理宽度 × 1.1、高度给到 2.2 倍宽度（足够容纳竖版）来解码，
/// 纹理上屏时基本 1:1 或轻微降采样，清晰度直接吃满源图。
struct CoverTexBounds {
    int maxW = 320;
    int maxH = 180;
};

CoverTexBounds coverTextureBounds(float frameLogicalWidth) {
    float scale = 1.0f;
#ifdef __SWITCH__
    scale = brls::Application::windowScale;
#endif
    if (!(scale > 0.0f) || scale > 3.0f) {
        scale = 1.0f;
    }
    constexpr float kOversample = 1.1f; // 少量过采样，FILL 裁切 + 缩放后仍清晰
    CoverTexBounds b;
    b.maxW = static_cast<int>(std::lround(frameLogicalWidth * scale * kOversample));
    b.maxH = static_cast<int>(std::lround(frameLogicalWidth * scale * kOversample * 2.2f));
    b.maxW = std::clamp(b.maxW, 320, 768);
    b.maxH = std::clamp(b.maxH, 180, 1024);
    return b;
}

// 大厅卡片封面控件的逻辑尺寸（与 rebuildGameGrid 里的布局保持一处定义）
constexpr float kCardCoverWidth = 290.0f;
constexpr float kCardCoverHeight = 162.0f;
constexpr float kDetailCoverWidth = 260.0f;
constexpr float kDetailCoverHeight = 146.0f;

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

std::string detectDisplayBadge(int handheldResolution) {
    const int hhRes = normalizeHandheldResolution(handheldResolution);
#ifdef __SWITCH__
    if (appletGetOperationMode() == AppletOperationMode_Console) {
        return "底座 1080p60";
    }
    return hhRes == AppConfig::kHandheldRes1080 ? "掌机 1080p60" : "掌机 720p60";
#else
    return hhRes == AppConfig::kHandheldRes1080 ? "桌面 1080p60" : "桌面 720p60";
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
    if (m_prewarmCancel) {
        m_prewarmCancel->store(true);
    }
    if (m_windowSizeSubValid) {
        brls::Application::getWindowSizeChangedEvent()->unsubscribe(m_windowSizeSub);
        m_windowSizeSubValid = false;
    }
    if (m_windowFocusSubValid) {
        brls::Application::getWindowFocusChangedEvent()->unsubscribe(m_windowFocusSub);
        m_windowFocusSubValid = false;
    }
    stopCoverWorker();
    if (m_qrThread.joinable()) {
        m_qrThread.join();
    }
    if (m_netThread.joinable()) {
        m_netThread.join();
    }
    if (m_prewarmThread.joinable()) {
        m_prewarmThread.join();
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

    m_isDarkTheme = syncSystemDarkTheme();
    const LobbyPalette& pal = currentLobbyPalette(m_isDarkTheme);

    m_root = new brls::Box(brls::Axis::COLUMN);
    m_root->setBackgroundColor(pal.pageBg);
    m_root->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_root->setAlignItems(brls::AlignItems::STRETCH);

#ifdef __SWITCH__
    const AppletType appletType = appletGetAppletType();
    if (appletType != AppletType_Application && appletType != AppletType_SystemApplication) {
        auto* banner = new brls::Box(brls::Axis::ROW);
        banner->setBackgroundColor(pal.bannerWarnBg);
        banner->setPadding(6.0f, 24.0f, 6.0f, 24.0f);
        banner->setJustifyContent(brls::JustifyContent::CENTER);

        auto* warn = new brls::Label();
        warn->setText(
            "警告：当前为相册模式(低内存，硬解易崩溃)。请退回桌面按住 R 打开任意游戏以满血模式启动！");
        warn->setFontSize(14.5f);
        warn->setTextColor(nvgRGB(255, 255, 255));
        banner->addView(warn);
        m_root->addView(banner);
    }
#endif

    // 顶部导航栏：官方圆角图标 + 品牌标题 + 模式角标 + 胶囊搜索框 + 账号胶囊 + 菜单按钮
    m_header = new brls::Box(brls::Axis::ROW);
    m_header->setHeight(64.0f);
    m_header->setPadding(0.0f, 24.0f, 0.0f, 24.0f);
    m_header->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    m_header->setAlignItems(brls::AlignItems::CENTER);
    m_header->setLineBottom(1.0f);

    auto* leftGroup = new brls::Box(brls::Axis::ROW);
    leftGroup->setAlignItems(brls::AlignItems::CENTER);

    auto* brandIcon = new brls::Image();
    brandIcon->setDimensions(34.0f, 34.0f);
    brandIcon->setScalingType(brls::ImageScalingType::FIT);
    brandIcon->setImageFromRes("img/levelup_96.png");
    brandIcon->setMarginRight(10.0f);
    leftGroup->addView(brandIcon);

    m_brandLabel = new brls::Label();
    m_brandLabel->setText("LevelUp-NX");
    m_brandLabel->setFontSize(22.0f);
    m_brandLabel->setMarginRight(10.0f);
    leftGroup->addView(m_brandLabel);

    m_modeBadgeBox = new StyledSurfaceBox(brls::Axis::ROW);
    m_modeBadgeBox->setHeight(26.0f);
    m_modeBadgeBox->setPadding(0.0f, 10.0f, 0.0f, 10.0f);
    m_modeBadgeBox->setAlignItems(brls::AlignItems::CENTER);
    m_modeBadgeBox->setSurfaceRadius(13.0f);
    m_modeBadgeBox->setSurfaceBorderWidth(0.0f);
    m_modeBadgeBox->setMarginRight(12.0f);

    m_modeBadge = new brls::Label();
    m_modeBadge->setText(detectDisplayBadge(m_cfg.handheldResolution));
    m_modeBadge->setFontSize(12.5f);
    m_modeBadgeBox->addView(m_modeBadge);
    leftGroup->addView(m_modeBadgeBox);

    m_searchBox = new StyledSurfaceBox(brls::Axis::ROW);
    m_searchBox->setHeight(38.0f);
    m_searchBox->setMinWidth(176.0f);
    m_searchBox->setMaxWidth(260.0f);
    m_searchBox->setPadding(0.0f, 14.0f, 0.0f, 14.0f);
    m_searchBox->setSurfaceRadius(19.0f);
    m_searchBox->setSurfaceBorderWidth(1.0f);
    m_searchBox->setHighlightCornerRadius(19.0f);
    m_searchBox->setHighlightPadding(2.0f);
    m_searchBox->setHideHighlightBackground(true);
    m_searchBox->setAlignItems(brls::AlignItems::CENTER);
    m_searchBox->setFocusable(true);
    m_searchBox->registerClickAction([this](brls::View*) {
        openSearchIme();
        return true;
    });
    m_searchBox->addGestureRecognizer(new brls::TapGestureRecognizer(m_searchBox));

    m_searchIcon = new VectorIconView(VectorIconKind::Search, 15.0f, pal.inkTertiary);
    m_searchIcon->setMarginRight(8.0f);
    m_searchBox->addView(m_searchIcon);

    m_searchChip = new brls::Label();
    m_searchChip->setFontSize(13.5f);
    m_searchChip->setSingleLine(true);
    m_searchChip->setText("搜索游戏 (Y)");
    m_searchBox->addView(m_searchChip);
    leftGroup->addView(m_searchBox);

    m_header->addView(leftGroup);

    auto* rightGroup = new brls::Box(brls::Axis::ROW);
    rightGroup->setAlignItems(brls::AlignItems::CENTER);

    m_accountBox = new StyledSurfaceBox(brls::Axis::ROW);
    m_accountBox->setHeight(38.0f);
    m_accountBox->setPadding(0.0f, 14.0f, 0.0f, 14.0f);
    m_accountBox->setSurfaceRadius(19.0f);
    m_accountBox->setSurfaceBorderWidth(1.0f);
    m_accountBox->setHighlightCornerRadius(19.0f);
    m_accountBox->setHighlightPadding(2.0f);
    m_accountBox->setHideHighlightBackground(true);
    m_accountBox->setAlignItems(brls::AlignItems::CENTER);
    m_accountBox->setMarginRight(10.0f);
    m_accountBox->setFocusable(true);
    m_accountBox->registerClickAction([this](brls::View*) {
        if (m_cfg.cred.sessionKey.empty()) {
            openQrLoginModal();
        } else {
            openAccountModal();
        }
        return true;
    });
    m_accountBox->addGestureRecognizer(new brls::TapGestureRecognizer(m_accountBox));

    m_accountIcon = new VectorIconView(VectorIconKind::Key, 17.0f, pal.accountGuestIcon);
    m_accountIcon->setMarginRight(7.0f);
    m_accountBox->addView(m_accountIcon);

    m_accountLabel = new brls::Label();
    m_accountLabel->setFontSize(13.5f);
    m_accountLabel->setSingleLine(true);
    m_accountLabel->setMaxWidth(360.0f);
    m_accountBox->addView(m_accountLabel);
    rightGroup->addView(m_accountBox);

    m_menuBox = new StyledSurfaceBox(brls::Axis::ROW);
    m_menuBox->setHeight(38.0f);
    m_menuBox->setPadding(0.0f, 15.0f, 0.0f, 15.0f);
    m_menuBox->setSurfaceRadius(19.0f);
    m_menuBox->setSurfaceBorderWidth(1.0f);
    m_menuBox->setHighlightCornerRadius(19.0f);
    m_menuBox->setHighlightPadding(2.0f);
    m_menuBox->setHideHighlightBackground(true);
    m_menuBox->setAlignItems(brls::AlignItems::CENTER);
    m_menuBox->setFocusable(true);
    m_menuBox->registerClickAction([this](brls::View*) {
        openQuickMenuModal();
        return true;
    });
    m_menuBox->addGestureRecognizer(new brls::TapGestureRecognizer(m_menuBox));

    m_menuIcon = new VectorIconView(VectorIconKind::Menu, 16.0f, pal.inkPrimary);
    m_menuIcon->setMarginRight(8.0f);
    m_menuBox->addView(m_menuIcon);

    m_menuLabel = new brls::Label();
    m_menuLabel->setText("菜单");
    m_menuLabel->setFontSize(14.0f);
    m_menuBox->addView(m_menuLabel);
    rightGroup->addView(m_menuBox);

    m_header->addView(rightGroup);
    m_root->addView(m_header);

    // 分类药丸栏
    m_categoryStrip = new brls::Box(brls::Axis::ROW);
    m_categoryStrip->setHeight(50.0f);
    m_categoryStrip->setPadding(9.0f, 24.0f, 9.0f, 24.0f);
    m_categoryStrip->setAlignItems(brls::AlignItems::CENTER);
    m_categoryStrip->setLineBottom(1.0f);
    m_root->addView(m_categoryStrip);

    // 游戏网格滚动区
    m_scrollFrame = new brls::ScrollingFrame();
    m_scrollFrame->setGrow(1.0f);
    m_scrollFrame->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    m_root->addView(m_scrollFrame);

    // 底栏按键提示、居中署名信息 & 游戏计数
    m_footer = new brls::Box(brls::Axis::ROW);
    m_footer->setHeight(40.0f);
    m_footer->setPadding(0.0f, 24.0f, 0.0f, 24.0f);
    m_footer->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    m_footer->setAlignItems(brls::AlignItems::CENTER);
    m_footer->setLineTop(1.0f);

    m_hintLabel = new brls::Label();
    m_hintLabel->setText(
        "A 详情/启动  Y 搜索  X 账号/登录  L/R 分类  ZL/ZR 翻页  + 设置  - 刷新");
    m_hintLabel->setFontSize(13.0f);
    m_hintLabel->setSingleLine(true);
    m_footer->addView(m_hintLabel);

    m_creditBar = new brls::Box(brls::Axis::ROW);
    m_creditBar->setHeight(28.0f);
    m_creditBar->setPadding(0.0f, 10.0f, 0.0f, 10.0f);
    m_creditBar->setAlignItems(brls::AlignItems::CENTER);
    m_creditBar->setJustifyContent(brls::JustifyContent::CENTER);
    m_creditBar->setFocusable(true);
    m_creditBar->setHideHighlightBackground(true);
    m_creditBar->setHighlightCornerRadius(8.0f);

    auto* biliBadge = new brls::Box(brls::Axis::ROW);
    biliBadge->setPadding(1.5f, 7.0f, 1.5f, 7.0f);
    biliBadge->setCornerRadius(5.0f);
    biliBadge->setBackgroundColor(pal.biliPink);
    biliBadge->setMarginRight(7.0f);
    auto* biliBadgeLabel = new brls::Label();
    biliBadgeLabel->setText("bilibili");
    biliBadgeLabel->setFontSize(11.5f);
    biliBadgeLabel->setTextColor(nvgRGB(255, 255, 255));
    biliBadge->addView(biliBadgeLabel);
    m_creditBar->addView(biliBadge);

    m_creditPrefix = new brls::Label();
    m_creditPrefix->setText("B站 UP主：");
    m_creditPrefix->setFontSize(12.5f);
    m_creditPrefix->setSingleLine(true);
    m_creditBar->addView(m_creditPrefix);

    m_creditName = new brls::Label();
    m_creditName->setText("牛巨眼");
    m_creditName->setFontSize(13.5f);
    m_creditName->setSingleLine(true);
    m_creditBar->addView(m_creditName);

    m_creditSuffix = new brls::Label();
    m_creditSuffix->setText("  ·  客户端由 UP 主制作分享，欢迎关注支持");
    m_creditSuffix->setFontSize(12.0f);
    m_creditSuffix->setSingleLine(true);
    m_creditBar->addView(m_creditSuffix);

    m_creditBar->registerClickAction([this](brls::View*) {
        const auto now = std::chrono::steady_clock::now();
        if (m_lastCreditTap.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastCreditTap).count() > 2500) {
            m_creditTapCount = 0;
        }
        m_lastCreditTap = now;
        if (++m_creditTapCount >= 20) {
            m_qMenuUnlocked = true;
        }
        brls::Application::notify("B站 搜索：牛巨眼");
        return true;
    });
    m_creditBar->addGestureRecognizer(new brls::TapGestureRecognizer(m_creditBar));
    m_footer->addView(m_creditBar);

    m_countLabel = new brls::Label();
    m_countLabel->setFontSize(13.0f);
    m_countLabel->setSingleLine(true);
    m_footer->addView(m_countLabel);

    m_root->addView(m_footer);

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

    // 掌机/底座切换后按新的实际像素重解码封面（Activity::onWindowSizeChanged 非虚函数）
    m_windowSizeSub = brls::Application::getWindowSizeChangedEvent()->subscribe([this]() {
        handleDisplayScaleChanged();
    });
    m_windowSizeSubValid = true;

    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    m_windowFocusSub = brls::Application::getWindowFocusChangedEvent()->subscribe(
        [this, alive](bool focused) {
            if (!focused) {
                return;
            }
            brls::sync([this, alive]() {
                if (!alive->load()) {
                    return;
                }
                syncThemeFromSystem(false);
            });
        });
    m_windowFocusSubValid = true;

    applyChromeTheme();
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
    startDialPrewarm();
}

// 大厅预热测速：此时既没有云实例、也没有 flow，探测只打各区域的测速网关，
// 不消耗云游戏时长；结果落盘后，下一次点「启动」时 apply 就能带上新鲜的全区域延迟表。
void LevelUpLobbyActivity::startDialPrewarm() {
    if (m_prewarmStarted.exchange(true)) {
        return;
    }
    if (m_cfg.lpUuid.empty() || m_cfg.lastPkgName <= 0) {
        logInfo("[Lobby] dial prewarm skipped (need one streaming session first)");
        return;
    }
    m_prewarmCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_prewarmCancel;
    const std::string lpUuid = m_cfg.lpUuid;
    const int64_t pkg = m_cfg.lastPkgName;
    logInfo("[Lobby] dial prewarm start uuid=" + lpUuid + " pkg=" + std::to_string(pkg));
    if (m_prewarmThread.joinable()) {
        m_prewarmThread.join();
    }
    m_prewarmThread = std::thread([lpUuid, pkg, cancel]() {
        ytcx::dialPrewarm(lpUuid, pkg, ytcx::kDialCachePath, cancel.get());
    });
}

void LevelUpLobbyActivity::syncThemeFromSystem(bool forceRebuild) {
    const bool dark = syncSystemDarkTheme();
    if (dark == m_isDarkTheme && !forceRebuild) {
        return;
    }
    m_isDarkTheme = dark;
    applyChromeTheme();
    updateHeaderUi();
    if (isForegroundActivity()) {
        rebuildCategoryStrip();
        rebuildGameGrid();
    } else {
        m_needsGridRebuild = true;
    }
}

void LevelUpLobbyActivity::applyChromeTheme() {
    const LobbyPalette& pal = currentLobbyPalette(m_isDarkTheme);
    if (m_root) {
        m_root->setBackgroundColor(pal.pageBg);
    }
    if (m_creditBar) {
        m_creditBar->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
    }
    if (m_creditPrefix) {
        m_creditPrefix->setTextColor(pal.inkSecondary);
    }
    if (m_creditName) {
        m_creditName->setTextColor(pal.biliPink);
    }
    if (m_creditSuffix) {
        m_creditSuffix->setTextColor(pal.inkTertiary);
    }
    if (m_header) {
        m_header->setBackgroundColor(pal.headerBg);
        m_header->setLineColor(pal.hairline);
    }
    if (m_brandLabel) {
        m_brandLabel->setTextColor(pal.inkPrimary);
    }
    if (m_modeBadgeBox) {
        m_modeBadgeBox->setSurfaceColors(pal.modeBadgeBg, nvgRGBA(0, 0, 0, 0));
    }
    if (m_modeBadge) {
        m_modeBadge->setTextColor(pal.modeBadgeText);
    }
    if (m_menuBox) {
        m_menuBox->setSurfaceColors(pal.surfacePill, pal.surfacePillBorder, pal.cardBorderFocused);
        m_menuBox->setSurfaceShadow(
            true, pal.cardShadowInner, pal.cardShadowFocused, 2.0f, 8.0f, 0.5f);
    }
    if (m_menuIcon) {
        m_menuIcon->setColor(pal.inkPrimary);
    }
    if (m_menuLabel) {
        m_menuLabel->setTextColor(pal.inkPrimary);
    }
    if (m_categoryStrip) {
        m_categoryStrip->setBackgroundColor(pal.catBarBg);
        m_categoryStrip->setLineColor(pal.hairline);
    }
    if (m_footer) {
        m_footer->setBackgroundColor(pal.footerBg);
        m_footer->setLineColor(pal.hairline);
    }
    if (m_hintLabel) {
        m_hintLabel->setTextColor(pal.inkTertiary);
    }
    if (m_countLabel) {
        m_countLabel->setTextColor(pal.accentText);
    }
}

void LevelUpLobbyActivity::onResume() {
    // 回到大厅时把震动归零一次：上一局串流的强度、或 borealis clearVibration() 在
    // 手柄 style 变化时误写的满幅，都会让手柄在大厅里一直震。
    input::zeroAllVibrationDevices("lobby-resume");
    m_recent = m_store.loadRecent(m_cfg.cred.userId);
    const bool prevDark = m_isDarkTheme;
    const bool nowDark = syncSystemDarkTheme();
    const bool themeChanged = (nowDark != prevDark);
    if (themeChanged) {
        m_isDarkTheme = nowDark;
        applyChromeTheme();
        m_needsGridRebuild = true;
    }
    updateHeaderUi();
    if (m_selectedCategory == "__RECENT__" || m_needsGridRebuild || themeChanged) {
        m_needsGridRebuild = false;
        std::shared_ptr<std::atomic<bool>> alive = m_alive;
        brls::sync([this, alive]() {
            if (!alive->load()) return;
            if (!isForegroundActivity()) {
                m_needsGridRebuild = true;
                return;
            }
            rebuildCategoryStrip();
            rebuildGameGrid();
        });
    }
}

void LevelUpLobbyActivity::handleDisplayScaleChanged() {
    // 掌机 720p (uiScale 1.0) <-> 底座 1080p (uiScale 1.5) 切换后，卡片实际像素变了，
    // 按旧预算解码的封面会发糊，这里重建网格并按新预算重解码封面纹理。
    // 该回调由 Application::onWindowResized 通过 brls::delay 投递，运行在主线程。
    if (isExiting() || !m_alive->load() || !m_scrollFrame) {
        return;
    }
    const auto& stack = brls::Application::getActivitiesStack();
    if (stack.empty() || stack.back() != this) {
        return; // 串流/弹窗期间不折腾纹理
    }
    updateHeaderUi();
    const float scale = brls::Application::windowScale;
    if (std::fabs(m_coverTexScale - scale) <= 0.01f) {
        return;
    }
    logInfo("[Lobby] UI scale changed to " + std::to_string(scale) + ", re-decoding covers");
    rebuildGameGrid();
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

        if (isExiting() || !m_alive->load()) {
            return;
        }
        if (task.generation != m_gridGeneration.load()) {
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
        if (!decodeDownscaleToRgba(bytes.data(),
                                   bytes.size(),
                                   task.texMaxW > 0 ? task.texMaxW : 320,
                                   task.texMaxH > 0 ? task.texMaxH : 180,
                                   rgba,
                                   imgW,
                                   imgH)) {
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

void LevelUpLobbyActivity::enqueueCover(
    const GameEntry& game, uint64_t gen, brls::Image* target, int texMaxW, int texMaxH) {
    if (!target || (game.gameId.empty() && game.coverUrl.empty())) {
        return;
    }
    {
        std::lock_guard lock(m_coverMu);
        if (m_coverStop) {
            return;
        }
        CoverTask task;
        task.gameId = game.gameId;
        task.url = game.coverUrl;
        task.generation = gen;
        task.target = target;
        task.texMaxW = texMaxW;
        task.texMaxH = texMaxH;
        m_coverQueue.push_back(std::move(task));
    }
    m_coverCv.notify_one();
}

void LevelUpLobbyActivity::updateHeaderUi() {
    const LobbyPalette& pal = currentLobbyPalette(m_isDarkTheme);

    if (m_modeBadge) {
        m_modeBadge->setText(detectDisplayBadge(m_cfg.handheldResolution));
        m_modeBadge->setTextColor(pal.modeBadgeText);
    }
    if (m_modeBadgeBox) {
        m_modeBadgeBox->setSurfaceColors(pal.modeBadgeBg, nvgRGBA(0, 0, 0, 0));
    }

    if (m_searchChip) {
        if (m_searchQuery.empty()) {
            m_searchChip->setText("搜索游戏 (Y)");
            m_searchChip->setTextColor(pal.inkTertiary);
            if (m_searchIcon) {
                m_searchIcon->setColor(pal.inkTertiary);
            }
            if (m_searchBox) {
                m_searchBox->setSurfaceColors(
                    pal.surfacePill, pal.surfacePillBorder, pal.cardBorderFocused);
            }
        } else {
            m_searchChip->setText("搜索: " + truncateUtf8(m_searchQuery, 16) + " (-清除)");
            m_searchChip->setTextColor(pal.accentText);
            if (m_searchIcon) {
                m_searchIcon->setColor(pal.accent);
            }
            if (m_searchBox) {
                m_searchBox->setSurfaceColors(
                    pal.accentSoftBg, pal.cardBorderFocused, pal.accent);
            }
        }
    }

    if (m_accountLabel) {
        if (m_cfg.cred.sessionKey.empty()) {
            if (m_accountIcon) {
                m_accountIcon->setKind(VectorIconKind::Key);
                m_accountIcon->setColor(pal.accountGuestIcon);
            }
            m_accountLabel->setText("扫码登录 (X)");
            m_accountLabel->setTextColor(pal.accountGuestText);
            if (m_accountBox) {
                m_accountBox->setSurfaceColors(
                    pal.accountGuestBg, pal.accountGuestBorder, pal.cardBorderFocused);
            }
        } else if (m_hasUserInfo) {
            if (m_accountIcon) {
                m_accountIcon->setKind(VectorIconKind::User);
                m_accountIcon->setColor(pal.accountAuthedIcon);
            }
            char buf[240];
            const std::string rawNick =
                !m_userInfo.nickName.empty() ? m_userInfo.nickName
                                             : ("UID " + std::to_string(m_userInfo.userId));
            const std::string nick = truncateUtf8(rawNick, 16);
            if (!m_userInfo.vipExpireTime.empty()) {
                std::snprintf(buf,
                              sizeof(buf),
                              "%s · VIP · HP %d分 · MP %d分",
                              nick.c_str(),
                              m_userInfo.hpMinutes(),
                              m_userInfo.mpMinutes());
            } else {
                std::snprintf(buf,
                              sizeof(buf),
                              "%s · HP %d分 · MP %d分",
                              nick.c_str(),
                              m_userInfo.hpMinutes(),
                              m_userInfo.mpMinutes());
            }
            m_accountLabel->setText(buf);
            m_accountLabel->setTextColor(pal.accountAuthedText);
            if (m_accountBox) {
                m_accountBox->setSurfaceColors(
                    pal.accountAuthedBg, pal.accountAuthedBorder, pal.cardBorderFocused);
            }
        } else {
            if (m_accountIcon) {
                m_accountIcon->setKind(VectorIconKind::User);
                m_accountIcon->setColor(pal.accountAuthedIcon);
            }
            const std::string rawWho = !m_cfg.cred.nickName.empty()
                                           ? m_cfg.cred.nickName
                                           : ("UID " + std::to_string(m_cfg.cred.userId));
            m_accountLabel->setText(truncateUtf8(rawWho, 18) + " · 账号 (X)");
            m_accountLabel->setTextColor(pal.accountAuthedText);
            if (m_accountBox) {
                m_accountBox->setSurfaceColors(
                    pal.accountAuthedBg, pal.accountAuthedBorder, pal.cardBorderFocused);
            }
        }
    }

    if (m_hintLabel) {
        if (m_cfg.cred.sessionKey.empty()) {
            m_hintLabel->setText(
                "A 详情/启动  Y 搜索  X 登录  L/R 分类  ZL/ZR 翻页  + 设置  - 刷新");
        } else {
            m_hintLabel->setText(
                "A 详情/启动  Y 搜索  X 账号  L/R 分类  ZL/ZR 翻页  + 设置  - 刷新");
        }
    }
}

bool LevelUpLobbyActivity::isForegroundActivity() const {
    const auto& stack = brls::Application::getActivitiesStack();
    return stack.empty() || stack.back() == this;
}

void LevelUpLobbyActivity::rebuildCategoryStrip() {
    if (!m_categoryStrip) {
        return;
    }
    if (!isForegroundActivity()) {
        m_needsGridRebuild = true;
        return;
    }
    const LobbyPalette& pal = currentLobbyPalette(m_isDarkTheme);
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
        auto* pill = new StyledSurfaceBox(brls::Axis::ROW);
        pill->setHeight(32.0f);
        pill->setPadding(0.0f, 11.0f, 0.0f, 11.0f);
        pill->setMarginRight(6.0f);
        pill->setAlignItems(brls::AlignItems::CENTER);
        pill->setJustifyContent(brls::JustifyContent::CENTER);
        pill->setSurfaceRadius(16.0f);
        if (active) {
            pill->setSurfaceColors(pal.accent, nvgRGBA(0, 0, 0, 0));
            pill->setSurfaceBorderWidth(0.0f);
            pill->setSurfaceShadow(
                true, pal.cardShadowFocused, nvgRGBA(0, 0, 0, 0), 2.0f, 8.0f, 0.5f);
        } else {
            pill->setSurfaceColors(pal.surfacePill, pal.surfacePillBorder);
            pill->setSurfaceBorderWidth(1.0f);
            pill->setSurfaceShadow(false, nvgRGBA(0, 0, 0, 0));
        }

        auto* lbl = new brls::Label();
        lbl->setText(p.label);
        lbl->setFontSize(13.5f);
        lbl->setTextColor(active ? nvgRGB(255, 255, 255) : pal.inkSecondary);
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
    if (!isForegroundActivity()) {
        m_needsGridRebuild = true;
        return;
    }

    const LobbyPalette& pal = currentLobbyPalette(m_isDarkTheme);
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
        m_countLabel->setTextColor(pal.accentText);
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
    gridBox->setPadding(16.0f, 28.0f, 24.0f, 28.0f);
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
        emptyLabel->setFontSize(19.0f);
        emptyLabel->setTextColor(pal.inkSecondary);
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
        // 卡片封面控件逻辑尺寸 290x162；按当前 UI 缩放换算成实际像素来解码封面纹理
        const CoverTexBounds covBounds = coverTextureBounds(kCardCoverWidth);
        if (std::fabs(m_coverTexScale - brls::Application::windowScale) > 0.01f) {
            m_coverTexScale = brls::Application::windowScale;
            logInfo("[Lobby] cover texture budget " + std::to_string(covBounds.maxW) + "x" +
                    std::to_string(covBounds.maxH) + " (uiScale=" +
                    std::to_string(m_coverTexScale) + ")");
        }
        for (size_t i = startIdx; i < endIdx; i += kCols) {
            auto* row = new brls::Box(brls::Axis::ROW);
            row->setHeight(brls::View::AUTO);
            row->setJustifyContent(brls::JustifyContent::FLEX_START);
            row->setMarginBottom(4.0f);

            for (size_t c = 0; c < kCols && (i + c) < endIdx; ++c) {
                const GameEntry& game = games[i + c];

                auto* card = new StyledSurfaceBox(brls::Axis::COLUMN);
                card->setWidth(290.0f);
                card->setHeight(230.0f);
                card->setMargins(8.0f, 8.0f, 8.0f, 8.0f);
                card->setSurfaceRadius(14.0f);
                card->setSurfaceColors(pal.cardBg, pal.cardBorder, pal.cardBorderFocused);
                card->setSurfaceBorderWidth(1.0f);
                card->setDrawBorderAfterChildren(true);
                card->setSurfaceShadow(
                    true, pal.cardShadowInner, pal.cardShadowFocused, 5.0f, 16.0f, 1.0f);
                card->setHighlightCornerRadius(16.0f);
                card->setHighlightPadding(2.0f);
                card->setHideHighlightBackground(true);
                card->setFocusable(true);

                // 顶部封面裁剪框：顶部左右保留 14px 圆角，底边随 imgFrame 裁平与文字区自然衔接
                auto* imgFrame = new brls::Box(brls::Axis::COLUMN);
                imgFrame->setWidth(kCardCoverWidth);
                imgFrame->setHeight(kCardCoverHeight);
                imgFrame->setBackgroundColor(pal.cardCoverPh);
                imgFrame->setCornerRadius(14.0f);
                imgFrame->setClipsToBounds(true);

                auto* img = new brls::Image();
                img->setDimensions(kCardCoverWidth, kCardCoverHeight + 16.0f);
                img->setScalingType(brls::ImageScalingType::FILL);
                img->setCornerRadius(14.0f);
                imgFrame->addView(img);
                card->addView(imgFrame);

                auto* info = new brls::Box(brls::Axis::COLUMN);
                info->setPadding(9.0f, 14.0f, 9.0f, 14.0f);
                info->setJustifyContent(brls::JustifyContent::CENTER);
                info->setGrow(1.0f);

                auto* title = new brls::Label();
                title->setText(game.name.empty() ? game.fullName : game.name);
                title->setFontSize(16.5f);
                title->setSingleLine(true);
                title->setTextColor(pal.inkPrimary);
                title->setMarginBottom(3.0f);
                info->addView(title);

                auto* tag = new brls::Label();
                tag->setText(joinGameCategories(game.categories));
                tag->setFontSize(12.5f);
                tag->setSingleLine(true);
                tag->setTextColor(pal.inkTertiary);
                info->addView(tag);

                card->setFocusAccentLabel(tag, pal.inkTertiary, pal.accentText);
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

                enqueueCover(game, gen, img, covBounds.maxW, covBounds.maxH);
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
                prevBtn->setTextColor(pal.inkTertiary);
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
            pageLbl->setFontSize(15.5f);
            pageLbl->setTextColor(pal.inkSecondary);
            pageLbl->setMarginRight(20.0f);
            pagerRow->addView(pageLbl);

            const bool hasNext = (m_currentPage + 1) < m_totalPages;
            auto* nextBtn = new brls::Button();
            nextBtn->setStyle(hasNext ? &brls::BUTTONSTYLE_PRIMARY : &brls::BUTTONSTYLE_DEFAULT);
            nextBtn->setText("下一页 (ZR)");
            if (!hasNext) {
                nextBtn->setTextColor(pal.inkTertiary);
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
    const LobbyPalette& pal = currentLobbyPalette(m_isDarkTheme);

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(pal.modalBackdrop);

    auto* panel = new StyledSurfaceBox(brls::Axis::COLUMN);
    panel->setWidth(680.0f);
    panel->setPadding(24.0f);
    panel->setSurfaceRadius(20.0f);
    panel->setSurfaceColors(pal.modalBg, pal.modalBorder);
    panel->setSurfaceBorderWidth(1.0f);
    panel->setSurfaceShadow(
        true, pal.modalShadowInner, nvgRGBA(0, 0, 0, 0), 12.0f, 30.0f, 2.0f);
    panel->setAlignItems(brls::AlignItems::STRETCH);

    auto* topRow = new brls::Box(brls::Axis::ROW);
    topRow->setMarginBottom(16.0f);

    auto* cover = new brls::Image();
    cover->setDimensions(kDetailCoverWidth, kDetailCoverHeight);
    cover->setScalingType(brls::ImageScalingType::FILL);
    cover->setCornerRadius(12.0f);
    cover->setMarginRight(18.0f);
    std::vector<uint8_t> coverBytes;
    if (m_store.loadCoverBytes(game.gameId, coverBytes) && coverBytes.size() > 16) {
        std::vector<uint8_t> rgba;
        int imgW = 0;
        int imgH = 0;
        const CoverTexBounds detailBounds = coverTextureBounds(kDetailCoverWidth);
        if (decodeDownscaleToRgba(coverBytes.data(),
                                  coverBytes.size(),
                                  detailBounds.maxW,
                                  detailBounds.maxH,
                                  rgba,
                                  imgW,
                                  imgH)) {
            applyRgbaTexture(cover, rgba, imgW, imgH);
        } else {
            logWarn("[Lobby] detail cover decode failed: " + game.gameId);
        }
    }
    topRow->addView(cover);

    auto* meta = new brls::Box(brls::Axis::COLUMN);
    meta->setGrow(1.0f);

    auto* title = new brls::Label();
    title->setText(game.name.empty() ? game.fullName : game.name);
    title->setFontSize(24.0f);
    title->setTextColor(pal.inkPrimary);
    title->setMarginBottom(6.0f);
    meta->addView(title);

    if (!game.fullName.empty() && game.fullName != game.name) {
        auto* sub = new brls::Label();
        sub->setText(game.fullName);
        sub->setFontSize(14.0f);
        sub->setTextColor(pal.inkSecondary);
        sub->setMarginBottom(8.0f);
        meta->addView(sub);
    }

    auto* catPill = new StyledSurfaceBox(brls::Axis::ROW);
    catPill->setPadding(4.0f, 10.0f, 4.0f, 10.0f);
    catPill->setSurfaceRadius(8.0f);
    catPill->setSurfaceColors(pal.accentSoftBg, nvgRGBA(0, 0, 0, 0));
    catPill->setSurfaceBorderWidth(0.0f);
    catPill->setAlignSelf(brls::AlignSelf::FLEX_START);

    auto* catLbl = new brls::Label();
    catLbl->setText("分类: " + joinGameCategories(game.categories) + "  ·  ID: " + game.gameId);
    catLbl->setFontSize(13.0f);
    catLbl->setTextColor(pal.accentText);
    catPill->addView(catLbl);
    meta->addView(catPill);

    topRow->addView(meta);
    panel->addView(topRow);

    if (!game.description.empty()) {
        auto* desc = new brls::Label();
        desc->setText(truncateUtf8(game.description, 260));
        desc->setFontSize(15.0f);
        desc->setSingleLine(false);
        desc->setTextColor(pal.inkSecondary);
        desc->setMarginBottom(14.0f);
        panel->addView(desc);
    }

    auto* cfgCard = new StyledSurfaceBox(brls::Axis::ROW);
    cfgCard->setPadding(10.0f, 14.0f, 10.0f, 14.0f);
    cfgCard->setMarginBottom(18.0f);
    cfgCard->setSurfaceRadius(12.0f);
    cfgCard->setSurfaceColors(pal.modalSurface, pal.hairline);
    cfgCard->setSurfaceBorderWidth(1.0f);

    char cfgBuf[160];
    std::snprintf(cfgBuf,
                  sizeof(cfgBuf),
                  "输出: %s  ·  HD震动: %s (%d%%)",
                  detectDisplayBadge(m_cfg.handheldResolution).c_str(),
                  m_cfg.rumbleEnabled ? "开" : "关",
                  m_cfg.rumbleStrength);
    auto* cfgLbl = new brls::Label();
    cfgLbl->setText(cfgBuf);
    cfgLbl->setFontSize(13.5f);
    cfgLbl->setTextColor(pal.inkSecondary);
    cfgCard->addView(cfgLbl);
    panel->addView(cfgCard);

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

    const bool isDarkSnapshot = m_isDarkTheme;
    const LobbyPalette& pal = currentLobbyPalette(isDarkSnapshot);

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(pal.modalBackdrop);

    auto* panel = new StyledSurfaceBox(brls::Axis::COLUMN);
    panel->setWidth(460.0f);
    panel->setPadding(24.0f);
    panel->setSurfaceRadius(20.0f);
    panel->setSurfaceColors(pal.modalBg, pal.modalBorder);
    panel->setSurfaceBorderWidth(1.0f);
    panel->setSurfaceShadow(
        true, pal.modalShadowInner, nvgRGBA(0, 0, 0, 0), 12.0f, 30.0f, 2.0f);
    panel->setAlignItems(brls::AlignItems::CENTER);

    auto* title = new brls::Label();
    title->setText("扫码登录 LevelUp 账号");
    title->setFontSize(22.0f);
    title->setTextColor(pal.inkPrimary);
    title->setMarginBottom(16.0f);
    panel->addView(title);

    auto* qrFrame = new StyledSurfaceBox(brls::Axis::COLUMN);
    qrFrame->setDimensions(268.0f, 268.0f);
    qrFrame->setPadding(8.0f);
    qrFrame->setSurfaceRadius(14.0f);
    qrFrame->setSurfaceColors(nvgRGB(255, 255, 255), pal.hairline);
    qrFrame->setSurfaceBorderWidth(1.0f);
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
    statusLbl->setTextColor(pal.inkSecondary);
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

    m_qrThread = std::thread([this, alive, modalOpen, qrImg, statusLbl, deviceId, isDarkSnapshot]() {
        api::Client client(deviceId, {});
        api::QRCodeResult qr;
        std::string err;
        if (!client.getQRCode(qr, &err)) {
            brls::sync([alive, modalOpen, statusLbl, err, isDarkSnapshot]() {
                if (!alive->load() || !modalOpen->load()) return;
                statusLbl->setText("获取二维码失败: " + err);
                statusLbl->setTextColor(currentLobbyPalette(isDarkSnapshot).dangerText);
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
                    isDarkSnapshot,
                    imgData = std::move(rgba)]() {
            if (!alive->load() || !modalOpen->load()) return;
            if (decodedQr) {
                applyRgbaTexture(qrImg, imgData, qrW, qrH);
            }
            statusLbl->setText("请使用微信 / LevelUp App 扫码登录");
            statusLbl->setTextColor(currentLobbyPalette(isDarkSnapshot).accentText);
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
                brls::sync([alive, modalOpen, statusLbl, isDarkSnapshot]() {
                    if (!alive->load() || !modalOpen->load()) return;
                    statusLbl->setText("已扫描，请在手机上点击确认登录...");
                    statusLbl->setTextColor(currentLobbyPalette(isDarkSnapshot).warnText);
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
                brls::sync([alive, modalOpen, statusLbl, isDarkSnapshot]() {
                    if (!alive->load() || !modalOpen->load()) return;
                    statusLbl->setText("二维码已过期，请点击取消后重试");
                    statusLbl->setTextColor(currentLobbyPalette(isDarkSnapshot).dangerText);
                });
                return;
            }
        }

        brls::sync([alive, modalOpen, statusLbl, isDarkSnapshot]() {
            if (!alive->load() || !modalOpen->load()) return;
            statusLbl->setText("扫码超时，请重试");
            statusLbl->setTextColor(currentLobbyPalette(isDarkSnapshot).dangerText);
        });
    });
}

void LevelUpLobbyActivity::openAccountModal() {
    const LobbyPalette& pal = currentLobbyPalette(m_isDarkTheme);

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(pal.modalBackdrop);

    auto* panel = new StyledSurfaceBox(brls::Axis::COLUMN);
    panel->setWidth(540.0f);
    panel->setPadding(24.0f);
    panel->setSurfaceRadius(20.0f);
    panel->setSurfaceColors(pal.modalBg, pal.modalBorder);
    panel->setSurfaceBorderWidth(1.0f);
    panel->setSurfaceShadow(
        true, pal.modalShadowInner, nvgRGBA(0, 0, 0, 0), 12.0f, 30.0f, 2.0f);
    panel->setAlignItems(brls::AlignItems::STRETCH);

    auto* headerRow = new brls::Box(brls::Axis::ROW);
    headerRow->setJustifyContent(brls::JustifyContent::CENTER);
    headerRow->setAlignItems(brls::AlignItems::CENTER);
    headerRow->setMarginBottom(14.0f);

    auto* avatarIcon = new VectorIconView(VectorIconKind::User, 24.0f, pal.accountAuthedIcon);
    avatarIcon->setMarginRight(10.0f);
    headerRow->addView(avatarIcon);

    auto* title = new brls::Label();
    title->setText("LevelUp 账号管理");
    title->setFontSize(22.0f);
    title->setTextColor(pal.inkPrimary);
    headerRow->addView(title);
    panel->addView(headerRow);

    auto* infoCard = new StyledSurfaceBox(brls::Axis::COLUMN);
    infoCard->setPadding(14.0f, 16.0f, 14.0f, 16.0f);
    infoCard->setMarginBottom(16.0f);
    infoCard->setSurfaceRadius(14.0f);
    infoCard->setSurfaceColors(pal.modalSurface, pal.hairline);
    infoCard->setSurfaceBorderWidth(1.0f);

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
        summary += "点击下方“刷新账号与在线游戏库”获取最新 VIP / HP / MP 时长。";
    }

    auto* infoLbl = new brls::Label();
    infoLbl->setText(summary);
    infoLbl->setFontSize(16.0f);
    infoLbl->setSingleLine(false);
    infoLbl->setTextColor(pal.inkPrimary);
    infoCard->addView(infoLbl);
    panel->addView(infoCard);

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

    auto* rescanBtn = makeBtn("切换账号 (扫码登录新账号)", &brls::BUTTONSTYLE_DEFAULT);
    rescanBtn->registerClickAction([this, alive, closeModal](brls::View*) {
        closeModal([this, alive]() {
            if (alive->load()) {
                openQrLoginModal();
            }
        });
        return true;
    });
    panel->addView(rescanBtn);

    auto* logoutBtn = makeBtn("退出登录 (登出当前账号)", &brls::BUTTONSTYLE_DEFAULT);
    logoutBtn->setTextColor(pal.dangerText);
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
            brls::Application::notify("已安全退出登录");
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

void LevelUpLobbyActivity::openQuickMenuModal() {
    const LobbyPalette& pal = currentLobbyPalette(m_isDarkTheme);

    float topPad = 104.0f;
#ifdef __SWITCH__
    const AppletType appletType = appletGetAppletType();
    if (appletType != AppletType_Application && appletType != AppletType_SystemApplication) {
        topPad = 136.0f;
    }
#endif

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::FLEX_START);
    backdrop->setAlignItems(brls::AlignItems::FLEX_END);
    backdrop->setPadding(topPad, 24.0f, 24.0f, 24.0f);
    backdrop->setBackgroundColor(pal.modalBackdrop);

    auto* panel = new StyledSurfaceBox(brls::Axis::COLUMN);
    panel->setWidth(328.0f);
    panel->setPadding(12.0f);
    panel->setSurfaceRadius(18.0f);
    panel->setSurfaceColors(pal.modalBg, pal.modalBorder);
    panel->setSurfaceBorderWidth(1.0f);
    panel->setSurfaceShadow(
        true, pal.modalShadowInner, nvgRGBA(0, 0, 0, 0), 12.0f, 28.0f, 2.0f);
    panel->setAlignItems(brls::AlignItems::STRETCH);

    auto* menuTitle = new brls::Label();
    menuTitle->setText(std::string("快捷菜单  ·  v") + APP_VERSION);
    menuTitle->setFontSize(12.5f);
    menuTitle->setTextColor(pal.inkTertiary);
    menuTitle->setMargins(2.0f, 8.0f, 8.0f, 8.0f);
    panel->addView(menuTitle);

    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    auto closed = std::make_shared<std::atomic<bool>>(false);
    auto closeMenu = [closed](std::function<void()> afterClose = nullptr) {
        if (!closed->exchange(true)) {
            brls::sync([after = std::move(afterClose)]() {
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
                if (after) {
                    after();
                }
            });
        }
    };

    auto addItem = [&](VectorIconKind iconKind,
                       NVGcolor iconCol,
                       const std::string& label,
                       const std::string& hint,
                       std::function<void()> onTap) -> StyledSurfaceBox* {
        auto* row = new StyledSurfaceBox(brls::Axis::ROW);
        row->setHeight(44.0f);
        row->setPadding(0.0f, 12.0f, 0.0f, 12.0f);
        row->setMarginBottom(6.0f);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        row->setSurfaceRadius(11.0f);
        row->setSurfaceColors(pal.modalSurface, pal.hairline, pal.cardBorderFocused);
        row->setSurfaceBorderWidth(1.0f);
        row->setHighlightCornerRadius(11.0f);
        row->setHighlightPadding(1.5f);
        row->setHideHighlightBackground(true);
        row->setFocusable(true);

        auto* left = new brls::Box(brls::Axis::ROW);
        left->setAlignItems(brls::AlignItems::CENTER);

        auto* icon = new VectorIconView(iconKind, 17.0f, iconCol);
        icon->setMarginRight(10.0f);
        left->addView(icon);

        auto* titleLbl = new brls::Label();
        titleLbl->setText(label);
        titleLbl->setFontSize(15.0f);
        titleLbl->setTextColor(pal.inkPrimary);
        left->addView(titleLbl);
        row->addView(left);

        auto* right = new brls::Box(brls::Axis::ROW);
        right->setAlignItems(brls::AlignItems::CENTER);
        if (!hint.empty()) {
            auto* hintLbl = new brls::Label();
            hintLbl->setText(hint);
            hintLbl->setFontSize(12.5f);
            hintLbl->setTextColor(pal.inkTertiary);
            hintLbl->setMarginRight(6.0f);
            right->addView(hintLbl);
        }
        right->addView(new VectorIconView(VectorIconKind::ChevronRight, 12.0f, pal.inkTertiary));
        row->addView(right);

        row->registerClickAction([onTap = std::move(onTap)](brls::View*) {
            onTap();
            return true;
        });
        row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
        panel->addView(row);
        return row;
    };

    auto* settingsItem = addItem(VectorIconKind::Settings, pal.accent, "设置中心", "+", [this, alive, closeMenu]() {
        closeMenu([this, alive]() {
            if (alive->load()) {
                openSettingsModal();
            }
        });
    });

    addItem(VectorIconKind::Guide, pal.accent, "操作说明", "按键指南", [this, alive, closeMenu]() {
        closeMenu([this, alive]() {
            if (alive->load()) {
                openControlsGuideModal();
            }
        });
    });

    if (m_cfg.cred.sessionKey.empty()) {
        addItem(VectorIconKind::Key, pal.accountGuestIcon, "扫码登录", "X", [this, alive, closeMenu]() {
            closeMenu([this, alive]() {
                if (alive->load()) {
                    openQrLoginModal();
                }
            });
        });
    } else {
        addItem(VectorIconKind::User, pal.accountAuthedIcon, "账号管理", "X", [this, alive, closeMenu]() {
            closeMenu([this, alive]() {
                if (alive->load()) {
                    openAccountModal();
                }
            });
        });
    }

    auto* divider = new brls::Box(brls::Axis::ROW);
    divider->setHeight(1.0f);
    divider->setMargins(4.0f, 6.0f, 10.0f, 6.0f);
    divider->setBackgroundColor(pal.hairline);
    panel->addView(divider);

    addItem(VectorIconKind::Refresh, pal.inkSecondary, "刷新在线游戏库", "-", [this, alive, closeMenu]() {
        closeMenu([this, alive]() {
            if (alive->load()) {
                refreshOnlineDataAsync(true);
            }
        });
    });

    addItem(VectorIconKind::Trash, pal.inkSecondary, "清除本地封面缓存", "清理", [this, alive, closeMenu]() {
        closeMenu([this, alive]() {
            if (!alive->load()) {
                return;
            }
            m_store.clearCoversCache();
            brls::Application::notify("已清除本地封面缓存");
            rebuildGameGrid();
        });
    });

#ifdef __SWITCH__
    auto* lastItem = addItem(VectorIconKind::HomeIcon, pal.inkSecondary, "安装到 Switch 桌面", "图标", [alive, closeMenu]() {
        closeMenu([alive]() {
            if (!alive->load()) {
                return;
            }
            const int rc = makeAppForwarder("LevelUp-NX", "LevelUp-Switch");
            if (rc == 0) {
                logInfo("[Forwarder] installed LevelUp-NX home menu forwarder");
                brls::Application::notify(
                    "已安装到 Switch 主界面（后续升级 NRO 无需重建），请返回桌面查看");
            } else {
                char buf[128];
                std::snprintf(
                    buf, sizeof(buf), "主界面安装失败 (0x%X)，需确认大气层签名补丁/sys-patch", rc);
                logWarn(std::string("[Forwarder] ") + buf);
                brls::Application::notify(buf);
            }
        });
    });
    lastItem->setMarginBottom(0.0f);
#endif

    backdrop->addView(panel);
    backdrop->addGestureRecognizer(new brls::TapGestureRecognizer(
        [panel, closeMenu](brls::TapGestureStatus status, brls::Sound*) {
            if (status.state == brls::GestureState::END &&
                !panel->getFrame().pointInside(status.position)) {
                closeMenu();
            }
        }));
    backdrop->registerAction("关闭", brls::BUTTON_B, [closeMenu](brls::View*) {
        closeMenu();
        return true;
    });

    brls::Application::pushActivity(new TranslucentModalActivity(backdrop),
                                    brls::TransitionAnimation::NONE);
    brls::Application::giveFocus(settingsItem);
}

void LevelUpLobbyActivity::openControlsGuideModal() {
    const LobbyPalette& pal = currentLobbyPalette(m_isDarkTheme);

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(pal.modalBackdrop);

    auto* panel = new StyledSurfaceBox(brls::Axis::COLUMN);
    panel->setWidth(800.0f);
    panel->setHeight(616.0f);
    panel->setPadding(24.0f);
    panel->setSurfaceRadius(20.0f);
    panel->setSurfaceColors(pal.modalBg, pal.modalBorder);
    panel->setSurfaceBorderWidth(1.0f);
    panel->setSurfaceShadow(
        true, pal.modalShadowInner, nvgRGBA(0, 0, 0, 0), 12.0f, 32.0f, 2.0f);
    panel->setAlignItems(brls::AlignItems::STRETCH);

    auto* headerRow = new brls::Box(brls::Axis::ROW);
    headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    headerRow->setAlignItems(brls::AlignItems::CENTER);
    headerRow->setMarginBottom(6.0f);

    auto* titleLeft = new brls::Box(brls::Axis::ROW);
    titleLeft->setAlignItems(brls::AlignItems::CENTER);
    auto* guideIcon = new VectorIconView(VectorIconKind::Guide, 24.0f, pal.accent);
    guideIcon->setMarginRight(10.0f);
    titleLeft->addView(guideIcon);

    auto* title = new brls::Label();
    title->setText("操作说明");
    title->setFontSize(23.0f);
    title->setTextColor(pal.inkPrimary);
    titleLeft->addView(title);
    headerRow->addView(titleLeft);

    auto* escPill = new StyledSurfaceBox(brls::Axis::ROW);
    escPill->setPadding(4.0f, 10.0f, 4.0f, 10.0f);
    escPill->setSurfaceRadius(10.0f);
    escPill->setSurfaceColors(pal.modalSurface, pal.hairline);
    escPill->setSurfaceBorderWidth(1.0f);
    auto* escLbl = new brls::Label();
    escLbl->setText("按 B 返回");
    escLbl->setFontSize(12.5f);
    escLbl->setTextColor(pal.inkSecondary);
    escPill->addView(escLbl);
    headerRow->addView(escPill);
    panel->addView(headerRow);

    auto* subtitle = new brls::Label();
    subtitle->setText("大厅导航、云游戏手柄按键组合、虚拟键盘与触屏鼠标模式的完整操作指引");
    subtitle->setFontSize(13.5f);
    subtitle->setTextColor(pal.inkSecondary);
    subtitle->setMarginBottom(12.0f);
    panel->addView(subtitle);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    auto* list = new brls::Box(brls::Axis::COLUMN);
    list->setHeight(brls::View::AUTO);
    list->setPadding(2.0f, 4.0f, 6.0f, 4.0f);
    list->setAlignItems(brls::AlignItems::STRETCH);

    struct GuideEntry {
        const char* keyText;
        const char* descText;
    };

    brls::View* firstFocusRow = nullptr;

    auto addSection = [&](const std::string& secTitle, const std::vector<GuideEntry>& entries) {
        auto* secHeader = new brls::Label();
        secHeader->setText(secTitle);
        secHeader->setFontSize(15.0f);
        secHeader->setTextColor(pal.accentText);
        secHeader->setMargins(4.0f, 4.0f, 6.0f, 4.0f);
        list->addView(secHeader);

        auto* card = new StyledSurfaceBox(brls::Axis::COLUMN);
        card->setPadding(6.0f, 12.0f, 6.0f, 12.0f);
        card->setMarginBottom(14.0f);
        card->setSurfaceRadius(14.0f);
        card->setSurfaceColors(pal.modalSurface, pal.hairline);
        card->setSurfaceBorderWidth(1.0f);
        card->setAlignItems(brls::AlignItems::STRETCH);

        for (size_t i = 0; i < entries.size(); ++i) {
            auto* row = new brls::Box(brls::Axis::ROW);
            row->setPadding(8.0f, 6.0f, 8.0f, 6.0f);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setFocusable(true);
            row->setHideHighlightBackground(true);
            row->setHighlightCornerRadius(10.0f);
            row->setHighlightPadding(1.0f);
            if (i > 0) {
                row->setLineTop(1.0f);
                row->setLineColor(pal.hairline);
            }

            auto* keyCap = new StyledSurfaceBox(brls::Axis::ROW);
            keyCap->setWidth(212.0f);
            keyCap->setShrink(0.0f);
            keyCap->setPadding(5.0f, 10.0f, 5.0f, 10.0f);
            keyCap->setMarginRight(14.0f);
            keyCap->setJustifyContent(brls::JustifyContent::CENTER);
            keyCap->setAlignItems(brls::AlignItems::CENTER);
            keyCap->setSurfaceRadius(8.0f);
            keyCap->setSurfaceColors(pal.cardBg, pal.surfacePillBorder);
            keyCap->setSurfaceBorderWidth(1.0f);

            auto* keyLbl = new brls::Label();
            keyLbl->setText(entries[i].keyText);
            keyLbl->setFontSize(13.0f);
            keyLbl->setHorizontalAlign(brls::HorizontalAlign::CENTER);
            keyLbl->setTextColor(pal.accentText);
            keyCap->addView(keyLbl);
            row->addView(keyCap);

            auto* descLbl = new brls::Label();
            descLbl->setText(entries[i].descText);
            descLbl->setFontSize(14.0f);
            descLbl->setSingleLine(false);
            descLbl->setGrow(1.0f);
            descLbl->setTextColor(pal.inkPrimary);
            row->addView(descLbl);

            if (!firstFocusRow) {
                firstFocusRow = row;
            }
            card->addView(row);
        }

        list->addView(card);
    };

    addSection("一 · 大厅操作",
               {
                   {"左摇杆 / 方向键", "移动焦点选择游戏卡片、搜索框、账号胶囊或菜单按钮"},
                   {"A / 触屏点击", "打开所选游戏详情页并启动云游戏"},
                   {"B", "关闭当前弹窗或返回上一级界面"},
                   {"Y / 点击搜索框", "呼出系统输入法搜索游戏（支持中文名 / 英文名 / 拼音关键字）"},
                   {"X / 点击账号胶囊", "未登录时打开微信/App 扫码登录；已登录时查看 VIP / HP / MP 时长与切换/登出"},
                   {"L / R（或点分类标签）", "切换上一分类 / 下一分类（全部、最近及各游戏分区）"},
                   {"ZL / ZR（或点翻页按钮）", "切换到上一页 / 下一页游戏列表"},
                   {"+ / 点击「菜单」按钮", "按 + 直达设置中心；点击右上角「菜单」可打开快捷菜单"},
                   {"-", "有搜索关键字时一键清空搜索；无搜索时刷新在线游戏库与账号状态"},
               });

    addSection("二 · 游戏中手柄与串流菜单",
               {
                   {"长按 - + +（0.4秒）", "呼出游戏内「串流菜单」（继续游戏、性能 HUD、强制刷新关键帧、HD震动、按键布局、触屏鼠标、退出云游戏）"},
                   {"同时按 L + R + - + +", "无需等待，立即呼出游戏内「串流菜单」"},
                   {"连接阶段按 B", "在画面尚未进入游戏前，随时取消当前云游戏连接并返回大厅"},
                   {"双摇杆 / L3 / R3 / 十字键", "完整透传摇杆、按下摇杆与方向键；设置内可开启「反转摇杆 Y 轴」"},
                   {"A / B / X / Y 与肩键", "映射标准动作键与 L / R / ZL / ZR / + / -；可在设置或串流菜单切换 Switch 原生或 Xbox（A-B / X-Y 互换）布局"},
                   {"掌机与无线手柄合并", "掌机 Joy-Con 与 1P 无线 Pro / Joy-Con 自动合并生效；支持 HD 震动开关与 0%–100% 强度调节"},
               });

    addSection("三 · 游戏中触屏悬浮按钮与鼠标模式",
               {
                   {"轻触屏幕任意位置", "在左上角显示「虚拟键盘」、右上角显示「鼠标模式」半透明悬浮按钮（无触屏操作 3 秒自动淡出）"},
                   {"左上角「虚拟键盘」", "呼出 Switch 系统输入法，输入的文本直接发送到云端游戏"},
                   {"右上角「鼠标模式」", "点击循环切换：点击模式 → 触控板模式 → 鼠标锁定（也可在设置与串流菜单切换）"},
                   {"点击模式（默认）", "触屏绝对坐标直点：触摸位置即移动指针并触发左键按住/拖拽，适合点选菜单"},
                   {"触控板模式", "单指滑动相对移动指针；单指轻点左键；双指轻点右键；双指上下滑动触发鼠标滚轮"},
                   {"鼠标锁定模式", "屏蔽触屏向游戏发送鼠标输入，仅保留悬浮按钮响应，防止掌机游玩时误触屏幕"},
               });

    scroll->setContentView(list);
    panel->addView(scroll);

    auto closed = std::make_shared<std::atomic<bool>>(false);
    auto closeModal = [closed]() {
        if (!closed->exchange(true)) {
            brls::sync([]() {
                brls::Application::popActivity(brls::TransitionAnimation::NONE);
            });
        }
    };

    auto* bottomRow = new brls::Box(brls::Axis::ROW);
    bottomRow->setMarginTop(12.0f);
    bottomRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    bottomRow->setAlignItems(brls::AlignItems::CENTER);

    auto* scrollHint = new brls::Label();
    scrollHint->setText("方向键 / 摇杆上下滚动浏览 · 触屏可上下滑动");
    scrollHint->setFontSize(13.0f);
    scrollHint->setTextColor(pal.inkTertiary);
    bottomRow->addView(scrollHint);

    auto* closeBtn = new brls::Button();
    closeBtn->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    closeBtn->setText("知道了 (B 返回)");
    closeBtn->setHeight(42.0f);
    closeBtn->setMinWidth(168.0f);
    closeBtn->registerClickAction([closeModal](brls::View*) {
        closeModal();
        return true;
    });
    bottomRow->addView(closeBtn);
    panel->addView(bottomRow);

    backdrop->addView(panel);
    backdrop->registerAction("返回", brls::BUTTON_B, [closeModal](brls::View*) {
        closeModal();
        return true;
    });

    brls::Application::pushActivity(new TranslucentModalActivity(backdrop),
                                    brls::TransitionAnimation::NONE);
    brls::Application::giveFocus(firstFocusRow ? firstFocusRow : static_cast<brls::View*>(closeBtn));
}

void LevelUpLobbyActivity::openSettingsModal() {
    const LobbyPalette& pal = currentLobbyPalette(m_isDarkTheme);

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(pal.modalBackdrop);

    auto* panel = new StyledSurfaceBox(brls::Axis::COLUMN);
    panel->setWidth(700.0f);
    panel->setHeight(620.0f);
    panel->setPadding(24.0f);
    panel->setSurfaceRadius(20.0f);
    panel->setSurfaceColors(pal.modalBg, pal.modalBorder);
    panel->setSurfaceBorderWidth(1.0f);
    panel->setSurfaceShadow(
        true, pal.modalShadowInner, nvgRGBA(0, 0, 0, 0), 12.0f, 30.0f, 2.0f);
    panel->setAlignItems(brls::AlignItems::STRETCH);

    auto* titleRow = new brls::Box(brls::Axis::ROW);
    titleRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    titleRow->setAlignItems(brls::AlignItems::CENTER);
    titleRow->setMarginBottom(12.0f);

    auto* titleLeft = new brls::Box(brls::Axis::ROW);
    titleLeft->setAlignItems(brls::AlignItems::CENTER);
    auto* setIcon = new VectorIconView(VectorIconKind::Settings, 22.0f, pal.accent);
    setIcon->setMarginRight(10.0f);
    titleLeft->addView(setIcon);

    auto* title = new brls::Label();
    title->setText("LevelUp-NX 设置");
    title->setFontSize(22.0f);
    title->setTextColor(pal.inkPrimary);
    titleLeft->addView(title);
    titleRow->addView(titleLeft);

    auto* verPill = new StyledSurfaceBox(brls::Axis::ROW);
    verPill->setPadding(4.0f, 10.0f, 4.0f, 10.0f);
    verPill->setSurfaceRadius(10.0f);
    verPill->setSurfaceColors(pal.accentSoftBg, nvgRGBA(0, 0, 0, 0));
    verPill->setSurfaceBorderWidth(0.0f);
    auto* verLbl = new brls::Label();
    verLbl->setText(std::string("v") + APP_VERSION);
    verLbl->setFontSize(12.5f);
    verLbl->setTextColor(pal.accentText);
    verPill->addView(verLbl);
    titleRow->addView(verPill);
    panel->addView(titleRow);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    auto* list = new brls::Box(brls::Axis::COLUMN);
    list->setHeight(brls::View::AUTO);
    list->setAlignItems(brls::AlignItems::STRETCH);

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

    auto* accountHeader = new brls::Header();
    accountHeader->setTitle("账号与登录");
    list->addView(accountHeader);

    auto* accountCell = new brls::DetailCell();
    if (m_cfg.cred.sessionKey.empty()) {
        accountCell->setText("当前账号状态");
        accountCell->setDetailText("未登录 (按 A 扫码登录)");
        accountCell->setDetailTextColor(pal.warnText);
        accountCell->registerClickAction([this, closeModal](brls::View*) {
            closeModal([this]() {
                openQrLoginModal();
            });
            return true;
        });
        list->addView(accountCell);

        auto* loginCell = new brls::DetailCell();
        loginCell->setText("微信 / App 扫码登录");
        loginCell->setDetailText("获取微信/App 登录二维码并登录");
        loginCell->setDetailTextColor(pal.accentText);
        loginCell->registerClickAction([this, closeModal](brls::View*) {
            closeModal([this]() {
                openQrLoginModal();
            });
            return true;
        });
        list->addView(loginCell);
    } else {
        const std::string who = !m_cfg.cred.nickName.empty()
                                    ? m_cfg.cred.nickName
                                    : ("UID: " + std::to_string(m_cfg.cred.userId));
        accountCell->setText("当前账号");
        accountCell->setDetailText(who + " (按 A 管理/查看详情)");
        accountCell->setDetailTextColor(pal.successText);
        accountCell->registerClickAction([this, closeModal](brls::View*) {
            closeModal([this]() {
                openAccountModal();
            });
            return true;
        });
        list->addView(accountCell);

        auto* logoutCell = new brls::DetailCell();
        logoutCell->setText("退出登录 (登出)");
        logoutCell->setDetailText("清除本机登录凭据并恢复未登录状态");
        logoutCell->setDetailTextColor(pal.dangerText);
        logoutCell->registerClickAction([this, closeModal](brls::View*) {
            closeModal([this]() {
                m_cfg.cred = {};
                m_hasUserInfo = false;
                m_userInfo = {};
                m_store.saveConfig(m_cfg);
                m_recent = m_store.loadRecent(0);
                updateHeaderUi();
                rebuildGameGrid();
                brls::Application::notify("已安全退出登录");
            });
            return true;
        });
        list->addView(logoutCell);
    }

    auto* guideHeader = new brls::Header();
    guideHeader->setTitle("使用帮助");
    list->addView(guideHeader);

    auto* guideCell = new brls::DetailCell();
    guideCell->setText("操作说明");
    guideCell->setDetailText("大厅与游戏按键 / 触屏指南");
    guideCell->setDetailTextColor(pal.accentText);
    guideCell->registerClickAction([this](brls::View*) {
        openControlsGuideModal();
        return true;
    });
    list->addView(guideCell);

    auto* streamHeader = new brls::Header();
    streamHeader->setTitle("串流与控制设置");
    list->addView(streamHeader);

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

    auto* resCell = new brls::SelectorCell();
    const int resIdx =
        normalizeHandheldResolution(m_cfg.handheldResolution) == AppConfig::kHandheldRes1080 ? 1 : 0;
    resCell->init(
        "掌机分辨率 (底座固定1080p)",
        {"720p (默认)", "1080p (超采样更清晰)"},
        resIdx,
        [this](int idx) {
            m_cfg.handheldResolution =
                (idx == 1) ? AppConfig::kHandheldRes1080 : AppConfig::kHandheldRes720;
            m_store.saveConfig(m_cfg);
            updateHeaderUi();
        });
    list->addView(resCell);

    if (m_qMenuUnlocked) {
        const int effLevel = resolveEffectiveVideoLevel(
            normalizeHandheldResolution(m_cfg.handheldResolution),
            m_cfg.videoLevelOverride);
        const int qIdx = (effLevel == 3) ? 1 : 0;
        auto* qCell = new brls::SelectorCell();
        qCell->init(
            "画质设置",
            {"2", "3"},
            qIdx,
            [this](int idx) {
                m_cfg.videoLevelOverride = (idx == 1) ? 3 : 2;
                m_store.saveConfig(m_cfg);
            });
        list->addView(qCell);
    }

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

    auto* xboxCell = new brls::BooleanCell();
    xboxCell->init("Xbox 按键布局 (A-B, X-Y 互换)", m_cfg.swapAbXy, [this](bool on) {
        m_cfg.swapAbXy = on;
        m_store.saveConfig(m_cfg);
    });
    list->addView(xboxCell);

    auto* mouseModeCell = new brls::SelectorCell();
    mouseModeCell->init(
        "触屏鼠标模式",
        {"点击模式（触屏直点）", "触控板模式（滑动/轻点）", "鼠标锁定（仅响应悬浮按钮）"},
        normalizeTouchMouseMode(m_cfg.touchMouseMode),
        [this](int idx) {
            m_cfg.touchMouseMode = normalizeTouchMouseMode(idx);
            m_store.saveConfig(m_cfg);
        });
    list->addView(mouseModeCell);

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
                brls::Application::notify("已安装到 Switch 主界面（后续升级 NRO 无需重建），请返回桌面查看");
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
        logoutBtn->setTextColor(pal.dangerText);
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
    brls::Application::giveFocus(guideCell);
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
