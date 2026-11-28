#pragma once

#include <borealis.hpp>

#include <algorithm>
#include <cmath>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace levelup::ui {

/// 贴合 Switch 原生浅/深色系统主题与 Apple 设计语言的配色表。
struct LobbyPalette {
    bool isDark = false;

    // 页面与骨架栏位
    NVGcolor pageBg{};
    NVGcolor creditBarBg{};
    NVGcolor headerBg{};
    NVGcolor catBarBg{};
    NVGcolor footerBg{};
    NVGcolor hairline{};

    // 卡片与图标占位
    NVGcolor cardBg{};
    NVGcolor cardCoverPh{};
    NVGcolor cardBorder{};
    NVGcolor cardBorderFocused{};
    NVGcolor cardShadowInner{};
    NVGcolor cardShadowFocused{};

    // 胶囊控件（搜索框、分类、菜单）
    NVGcolor surfacePill{};
    NVGcolor surfacePillBorder{};
    NVGcolor surfaceHover{};

    // 品牌与强调色（Apple System Blue）
    NVGcolor accent{};
    NVGcolor accentSoftBg{};
    NVGcolor accentText{};

    // 文字层级
    NVGcolor inkPrimary{};
    NVGcolor inkSecondary{};
    NVGcolor inkTertiary{};

    // 账号胶囊（已登录 / 未登录）
    NVGcolor accountAuthedBg{};
    NVGcolor accountAuthedBorder{};
    NVGcolor accountAuthedIcon{};
    NVGcolor accountAuthedText{};

    NVGcolor accountGuestBg{};
    NVGcolor accountGuestBorder{};
    NVGcolor accountGuestIcon{};
    NVGcolor accountGuestText{};

    // 显示模式角标
    NVGcolor modeBadgeBg{};
    NVGcolor modeBadgeText{};

    // 弹窗与浮层
    NVGcolor modalBackdrop{};
    NVGcolor modalBg{};
    NVGcolor modalSurface{};
    NVGcolor modalBorder{};
    NVGcolor modalShadowInner{};

    // 语义色
    NVGcolor dangerText{};
    NVGcolor warnText{};
    NVGcolor successText{};
    NVGcolor biliPink{};
    NVGcolor bannerWarnBg{};
};

inline const LobbyPalette& lightLobbyPalette() {
    static const LobbyPalette p{
        /*isDark*/ false,
        /*pageBg*/ nvgRGB(245, 245, 247),
        /*creditBarBg*/ nvgRGB(238, 240, 245),
        /*headerBg*/ nvgRGB(255, 255, 255),
        /*catBarBg*/ nvgRGB(250, 250, 252),
        /*footerBg*/ nvgRGB(252, 252, 254),
        /*hairline*/ nvgRGBA(15, 23, 42, 24),
        /*cardBg*/ nvgRGB(255, 255, 255),
        /*cardCoverPh*/ nvgRGB(234, 237, 243),
        /*cardBorder*/ nvgRGBA(15, 23, 42, 24),
        /*cardBorderFocused*/ nvgRGBA(0, 122, 255, 200),
        /*cardShadowInner*/ nvgRGBA(15, 23, 42, 34),
        /*cardShadowFocused*/ nvgRGBA(0, 122, 255, 52),
        /*surfacePill*/ nvgRGB(238, 240, 245),
        /*surfacePillBorder*/ nvgRGBA(15, 23, 42, 24),
        /*surfaceHover*/ nvgRGB(229, 233, 240),
        /*accent*/ nvgRGB(0, 122, 255),
        /*accentSoftBg*/ nvgRGB(232, 242, 255),
        /*accentText*/ nvgRGB(0, 102, 224),
        /*inkPrimary*/ nvgRGB(29, 29, 31),
        /*inkSecondary*/ nvgRGB(99, 102, 112),
        /*inkTertiary*/ nvgRGB(142, 142, 147),
        /*accountAuthedBg*/ nvgRGB(232, 247, 238),
        /*accountAuthedBorder*/ nvgRGBA(22, 163, 74, 62),
        /*accountAuthedIcon*/ nvgRGB(22, 163, 74),
        /*accountAuthedText*/ nvgRGB(20, 83, 45),
        /*accountGuestBg*/ nvgRGB(232, 242, 255),
        /*accountGuestBorder*/ nvgRGBA(0, 122, 255, 68),
        /*accountGuestIcon*/ nvgRGB(0, 122, 255),
        /*accountGuestText*/ nvgRGB(0, 98, 210),
        /*modeBadgeBg*/ nvgRGB(232, 242, 255),
        /*modeBadgeText*/ nvgRGB(0, 102, 224),
        /*modalBackdrop*/ nvgRGBA(15, 23, 42, 120),
        /*modalBg*/ nvgRGB(255, 255, 255),
        /*modalSurface*/ nvgRGB(244, 245, 248),
        /*modalBorder*/ nvgRGBA(15, 23, 42, 28),
        /*modalShadowInner*/ nvgRGBA(15, 23, 42, 68),
        /*dangerText*/ nvgRGB(220, 38, 38),
        /*warnText*/ nvgRGB(202, 104, 12),
        /*successText*/ nvgRGB(21, 128, 61),
        /*biliPink*/ nvgRGB(251, 114, 153),
        /*bannerWarnBg*/ nvgRGB(217, 119, 6),
    };
    return p;
}

inline const LobbyPalette& darkLobbyPalette() {
    static const LobbyPalette p{
        /*isDark*/ true,
        /*pageBg*/ nvgRGB(22, 23, 27),
        /*creditBarBg*/ nvgRGB(18, 19, 23),
        /*headerBg*/ nvgRGB(28, 30, 36),
        /*catBarBg*/ nvgRGB(25, 27, 32),
        /*footerBg*/ nvgRGB(24, 26, 31),
        /*hairline*/ nvgRGBA(255, 255, 255, 22),
        /*cardBg*/ nvgRGB(36, 39, 46),
        /*cardCoverPh*/ nvgRGB(28, 30, 36),
        /*cardBorder*/ nvgRGBA(255, 255, 255, 28),
        /*cardBorderFocused*/ nvgRGBA(10, 132, 255, 225),
        /*cardShadowInner*/ nvgRGBA(0, 0, 0, 120),
        /*cardShadowFocused*/ nvgRGBA(10, 132, 255, 72),
        /*surfacePill*/ nvgRGB(46, 50, 60),
        /*surfacePillBorder*/ nvgRGBA(255, 255, 255, 28),
        /*surfaceHover*/ nvgRGB(56, 61, 74),
        /*accent*/ nvgRGB(10, 132, 255),
        /*accentSoftBg*/ nvgRGBA(10, 132, 255, 44),
        /*accentText*/ nvgRGB(96, 182, 255),
        /*inkPrimary*/ nvgRGB(245, 245, 247),
        /*inkSecondary*/ nvgRGB(165, 170, 182),
        /*inkTertiary*/ nvgRGB(116, 122, 135),
        /*accountAuthedBg*/ nvgRGBA(34, 197, 94, 38),
        /*accountAuthedBorder*/ nvgRGBA(74, 222, 128, 72),
        /*accountAuthedIcon*/ nvgRGB(74, 222, 128),
        /*accountAuthedText*/ nvgRGB(220, 252, 231),
        /*accountGuestBg*/ nvgRGBA(10, 132, 255, 42),
        /*accountGuestBorder*/ nvgRGBA(96, 182, 255, 86),
        /*accountGuestIcon*/ nvgRGB(96, 182, 255),
        /*accountGuestText*/ nvgRGB(219, 239, 255),
        /*modeBadgeBg*/ nvgRGBA(10, 132, 255, 36),
        /*modeBadgeText*/ nvgRGB(96, 182, 255),
        /*modalBackdrop*/ nvgRGBA(0, 0, 0, 175),
        /*modalBg*/ nvgRGB(34, 36, 43),
        /*modalSurface*/ nvgRGB(44, 47, 56),
        /*modalBorder*/ nvgRGBA(255, 255, 255, 34),
        /*modalShadowInner*/ nvgRGBA(0, 0, 0, 165),
        /*dangerText*/ nvgRGB(255, 96, 96),
        /*warnText*/ nvgRGB(255, 198, 66),
        /*successText*/ nvgRGB(74, 222, 128),
        /*biliPink*/ nvgRGB(251, 114, 153),
        /*bannerWarnBg*/ nvgRGB(180, 105, 16),
    };
    return p;
}

inline const LobbyPalette& currentLobbyPalette(bool dark) {
    return dark ? darkLobbyPalette() : lightLobbyPalette();
}

/// 将 Borealis 内置浅/深色表调齐到 Switch 原生 + Apple 风调色板，保证按钮、列表项与焦点框风格统一。
inline void installLevelUpBorealisThemes() {
    auto& light = brls::Theme::getLightTheme();
    light.addColor("brls/background", nvgRGB(245, 245, 247));
    light.addColor("brls/text", nvgRGB(29, 29, 31));
    light.addColor("brls/text_disabled", nvgRGB(142, 142, 147));
    light.addColor("brls/accent", nvgRGB(0, 122, 255));
    light.addColor("brls/click_pulse", nvgRGBA(0, 122, 255, 36));
    light.addColor("brls/highlight/background", nvgRGB(255, 255, 255));
    light.addColor("brls/highlight/color1", nvgRGB(0, 122, 255));
    light.addColor("brls/highlight/color2", nvgRGB(72, 196, 255));
    light.addColor("brls/header/border", nvgRGB(224, 226, 232));
    light.addColor("brls/header/rectangle", nvgRGB(0, 122, 255));
    light.addColor("brls/header/subtitle", nvgRGB(116, 120, 128));
    light.addColor("brls/button/primary_enabled_background", nvgRGB(0, 122, 255));
    light.addColor("brls/button/primary_disabled_background", nvgRGB(196, 206, 220));
    light.addColor("brls/button/primary_enabled_text", nvgRGB(255, 255, 255));
    light.addColor("brls/button/primary_disabled_text", nvgRGB(240, 244, 250));
    light.addColor("brls/button/default_enabled_background", nvgRGB(238, 240, 245));
    light.addColor("brls/button/default_disabled_background", nvgRGB(244, 245, 248));
    light.addColor("brls/button/default_enabled_text", nvgRGB(29, 29, 31));
    light.addColor("brls/button/default_disabled_text", nvgRGB(142, 142, 147));
    light.addColor("brls/button/highlight_enabled_text", nvgRGB(0, 102, 224));
    light.addColor("brls/list/listItem_value_color", nvgRGB(0, 102, 224));

    auto& dark = brls::Theme::getDarkTheme();
    dark.addColor("brls/background", nvgRGB(34, 36, 43));
    dark.addColor("brls/text", nvgRGB(245, 245, 247));
    dark.addColor("brls/text_disabled", nvgRGB(116, 122, 135));
    dark.addColor("brls/accent", nvgRGB(10, 132, 255));
    dark.addColor("brls/click_pulse", nvgRGBA(10, 132, 255, 42));
    dark.addColor("brls/highlight/background", nvgRGB(44, 47, 56));
    dark.addColor("brls/highlight/color1", nvgRGB(10, 132, 255));
    dark.addColor("brls/highlight/color2", nvgRGB(84, 226, 255));
    dark.addColor("brls/header/border", nvgRGB(58, 62, 72));
    dark.addColor("brls/header/rectangle", nvgRGB(10, 132, 255));
    dark.addColor("brls/header/subtitle", nvgRGB(165, 170, 182));
    dark.addColor("brls/button/primary_enabled_background", nvgRGB(10, 132, 255));
    dark.addColor("brls/button/primary_disabled_background", nvgRGB(64, 70, 82));
    dark.addColor("brls/button/primary_enabled_text", nvgRGB(255, 255, 255));
    dark.addColor("brls/button/primary_disabled_text", nvgRGB(145, 152, 166));
    dark.addColor("brls/button/default_enabled_background", nvgRGB(50, 54, 64));
    dark.addColor("brls/button/default_disabled_background", nvgRGB(42, 45, 54));
    dark.addColor("brls/button/default_enabled_text", nvgRGB(245, 245, 247));
    dark.addColor("brls/button/default_disabled_text", nvgRGB(116, 122, 135));
    dark.addColor("brls/button/highlight_enabled_text", nvgRGB(96, 182, 255));
    dark.addColor("brls/list/listItem_value_color", nvgRGB(96, 182, 255));
}

/// 读取当前 Switch 系统主题（Basic White / Basic Black）并同步 Borealis 主题变体。
inline bool syncSystemDarkTheme() {
    const brls::ThemeVariant current = brls::Application::getThemeVariant();
    bool isDark = (current == brls::ThemeVariant::DARK);
#ifdef __SWITCH__
    ColorSetId colorSetId = ColorSetId_Light;
    if (R_SUCCEEDED(setsysGetColorSetId(&colorSetId))) {
        isDark = (colorSetId == ColorSetId_Dark);
        const brls::ThemeVariant target =
            isDark ? brls::ThemeVariant::DARK : brls::ThemeVariant::LIGHT;
        // SwitchPlatform::setThemeVariant 会调用 videoContext->recordStaticCommands()，
        // 必须在 createWindow() 完成初始化（NVGContext 就绪）且主题确实变化时才调用。
        if (target != current && brls::Application::getNVGContext() != nullptr) {
            if (auto* platform = brls::Application::getPlatform()) {
                platform->setThemeVariant(target);
            }
        }
    }
#endif
    return isDark;
}

/// 矢量图标类型：全部通过 NanoVG 几何路径绘制，绝不依赖系统字体里缺失的符号或 Emoji。
enum class VectorIconKind {
    Menu,
    User,
    Key,
    Search,
    Settings,
    Guide,
    Refresh,
    Trash,
    HomeIcon,
    ChevronRight,
};

/// NanoVG 矢量图标控件（抗锯齿、高分屏锐利、主题色可控）。
class VectorIconView : public brls::View {
  public:
    explicit VectorIconView(VectorIconKind kind = VectorIconKind::Menu,
                            float size = 18.0f,
                            NVGcolor color = nvgRGB(29, 29, 31))
        : m_kind(kind), m_color(color) {
        setIconSize(size);
    }

    void setKind(VectorIconKind kind) { m_kind = kind; }
    void setColor(NVGcolor color) { m_color = color; }

    void setIconSize(float size) {
        m_size = std::max(8.0f, size);
        setDimensions(m_size, m_size);
    }

    void draw(NVGcontext* vg,
              float x,
              float y,
              float width,
              float height,
              brls::Style,
              brls::FrameContext*) override {
        const float s = std::min(width, height);
        if (s <= 1.0f) {
            return;
        }
        const float cx = x + width * 0.5f;
        const float cy = y + height * 0.5f;
        const NVGcolor col = a(m_color);

        nvgSave(vg);
        switch (m_kind) {
        case VectorIconKind::Menu: {
            // 三道圆角横杠，居中对齐，完全不依赖 ☰ 字体字形
            const float barW = s * 0.78f;
            const float barH = std::max(1.8f, s * 0.125f);
            const float step = s * 0.27f;
            const float left = cx - barW * 0.5f;
            nvgFillColor(vg, col);
            for (int i = -1; i <= 1; ++i) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, left, cy + i * step - barH * 0.5f, barW, barH, barH * 0.5f);
                nvgFill(vg);
            }
            break;
        }
        case VectorIconKind::User: {
            // Apple SF-Symbols 风格人像头像圆框
            const float r = s * 0.44f;
            const float strokeW = std::max(1.3f, s * 0.085f);
            nvgBeginPath(vg);
            nvgCircle(vg, cx, cy, r);
            nvgStrokeWidth(vg, strokeW);
            nvgStrokeColor(vg, col);
            nvgStroke(vg);

            nvgBeginPath(vg);
            nvgCircle(vg, cx, cy - r * 0.22f, r * 0.30f);
            nvgFillColor(vg, col);
            nvgFill(vg);

            nvgBeginPath(vg);
            nvgArc(vg, cx, cy + r * 0.92f, r * 0.66f, 3.60f, 5.82f, NVG_CW);
            nvgClosePath(vg);
            nvgFill(vg);
            break;
        }
        case VectorIconKind::Key: {
            // 矢量钥匙图标（未登录胶囊）
            const float strokeW = std::max(1.4f, s * 0.095f);
            const float bowR = s * 0.20f;
            const float bowX = cx - s * 0.20f;
            nvgBeginPath(vg);
            nvgCircle(vg, bowX, cy, bowR);
            nvgStrokeWidth(vg, strokeW);
            nvgStrokeColor(vg, col);
            nvgStroke(vg);

            const float shaftStart = bowX + bowR;
            const float shaftEnd = cx + s * 0.38f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, shaftStart, cy);
            nvgLineTo(vg, shaftEnd, cy);
            nvgMoveTo(vg, shaftEnd - s * 0.14f, cy);
            nvgLineTo(vg, shaftEnd - s * 0.14f, cy + s * 0.16f);
            nvgMoveTo(vg, shaftEnd - s * 0.02f, cy);
            nvgLineTo(vg, shaftEnd - s * 0.02f, cy + s * 0.13f);
            nvgLineCap(vg, NVG_ROUND);
            nvgStroke(vg);
            break;
        }
        case VectorIconKind::Search: {
            // 放大镜矢量图标
            const float strokeW = std::max(1.5f, s * 0.105f);
            const float r = s * 0.28f;
            const float lx = cx - s * 0.09f;
            const float ly = cy - s * 0.09f;
            nvgBeginPath(vg);
            nvgCircle(vg, lx, ly, r);
            nvgMoveTo(vg, lx + r * 0.72f, ly + r * 0.72f);
            nvgLineTo(vg, cx + s * 0.37f, cy + s * 0.37f);
            nvgStrokeWidth(vg, strokeW);
            nvgStrokeColor(vg, col);
            nvgLineCap(vg, NVG_ROUND);
            nvgStroke(vg);
            break;
        }
        case VectorIconKind::Settings: {
            // Apple 风格三行滑块设置图标
            const float strokeW = std::max(1.4f, s * 0.09f);
            const float halfW = s * 0.38f;
            const float knobR = s * 0.105f;
            const float offsets[3] = {-s * 0.26f, 0.0f, s * 0.26f};
            const float knobs[3] = {-s * 0.14f, s * 0.16f, -s * 0.06f};
            nvgStrokeWidth(vg, strokeW);
            nvgStrokeColor(vg, col);
            nvgLineCap(vg, NVG_ROUND);
            for (int i = 0; i < 3; ++i) {
                const float ly = cy + offsets[i];
                const float kx = cx + knobs[i];
                nvgBeginPath(vg);
                nvgMoveTo(vg, cx - halfW, ly);
                nvgLineTo(vg, kx - knobR, ly);
                nvgMoveTo(vg, kx + knobR, ly);
                nvgLineTo(vg, cx + halfW, ly);
                nvgStroke(vg);

                nvgBeginPath(vg);
                nvgCircle(vg, kx, ly, knobR);
                nvgStroke(vg);
            }
            break;
        }
        case VectorIconKind::Guide: {
            // 手柄轮廓图标（操作说明）
            const float strokeW = std::max(1.4f, s * 0.085f);
            const float bw = s * 0.84f;
            const float bh = s * 0.56f;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - bw * 0.5f, cy - bh * 0.5f, bw, bh, bh * 0.42f);
            nvgStrokeWidth(vg, strokeW);
            nvgStrokeColor(vg, col);
            nvgStroke(vg);

            // 左侧十字方向
            const float dx = cx - bw * 0.23f;
            const float arm = s * 0.10f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, dx - arm, cy);
            nvgLineTo(vg, dx + arm, cy);
            nvgMoveTo(vg, dx, cy - arm);
            nvgLineTo(vg, dx, cy + arm);
            nvgLineCap(vg, NVG_ROUND);
            nvgStroke(vg);

            // 右侧动作键点
            const float rx = cx + bw * 0.22f;
            const float dotR = std::max(1.3f, s * 0.065f);
            nvgFillColor(vg, col);
            nvgBeginPath(vg);
            nvgCircle(vg, rx - s * 0.06f, cy + s * 0.05f, dotR);
            nvgCircle(vg, rx + s * 0.06f, cy - s * 0.05f, dotR);
            nvgFill(vg);
            break;
        }
        case VectorIconKind::Refresh: {
            // 环形刷新箭头
            const float strokeW = std::max(1.4f, s * 0.095f);
            const float r = s * 0.34f;
            nvgBeginPath(vg);
            nvgArc(vg, cx, cy, r, -0.55f, 4.35f, NVG_CW);
            nvgStrokeWidth(vg, strokeW);
            nvgStrokeColor(vg, col);
            nvgLineCap(vg, NVG_ROUND);
            nvgStroke(vg);

            const float ax = cx + r * std::cos(-0.55f);
            const float ay = cy + r * std::sin(-0.55f);
            nvgBeginPath(vg);
            nvgMoveTo(vg, ax - s * 0.14f, ay - s * 0.02f);
            nvgLineTo(vg, ax, ay);
            nvgLineTo(vg, ax + s * 0.01f, ay - s * 0.15f);
            nvgStroke(vg);
            break;
        }
        case VectorIconKind::Trash: {
            // 极简垃圾桶图标
            const float strokeW = std::max(1.4f, s * 0.088f);
            nvgStrokeWidth(vg, strokeW);
            nvgStrokeColor(vg, col);
            nvgLineCap(vg, NVG_ROUND);
            nvgLineJoin(vg, NVG_ROUND);

            const float topY = cy - s * 0.22f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - s * 0.34f, topY);
            nvgLineTo(vg, cx + s * 0.34f, topY);
            nvgMoveTo(vg, cx - s * 0.13f, topY);
            nvgLineTo(vg, cx - s * 0.10f, topY - s * 0.13f);
            nvgLineTo(vg, cx + s * 0.10f, topY - s * 0.13f);
            nvgLineTo(vg, cx + s * 0.13f, topY);
            nvgStroke(vg);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - s * 0.24f, topY, s * 0.48f, s * 0.56f, s * 0.07f);
            nvgStroke(vg);
            break;
        }
        case VectorIconKind::HomeIcon: {
            // 桌面图标（四宫格圆角方块）
            const float strokeW = std::max(1.3f, s * 0.085f);
            const float box = s * 0.28f;
            const float gap = s * 0.12f;
            nvgStrokeWidth(vg, strokeW);
            nvgStrokeColor(vg, col);
            for (int r = 0; r < 2; ++r) {
                for (int c = 0; c < 2; ++c) {
                    const float bx = cx - box - gap * 0.5f + c * (box + gap);
                    const float by = cy - box - gap * 0.5f + r * (box + gap);
                    nvgBeginPath(vg);
                    nvgRoundedRect(vg, bx, by, box, box, s * 0.07f);
                    nvgStroke(vg);
                }
            }
            break;
        }
        case VectorIconKind::ChevronRight: {
            // 右向轻箭头
            const float strokeW = std::max(1.4f, s * 0.10f);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - s * 0.14f, cy - s * 0.26f);
            nvgLineTo(vg, cx + s * 0.16f, cy);
            nvgLineTo(vg, cx - s * 0.14f, cy + s * 0.26f);
            nvgStrokeWidth(vg, strokeW);
            nvgStrokeColor(vg, col);
            nvgLineCap(vg, NVG_ROUND);
            nvgLineJoin(vg, NVG_ROUND);
            nvgStroke(vg);
            break;
        }
        }
        nvgRestore(vg);
    }

  private:
    VectorIconKind m_kind = VectorIconKind::Menu;
    float m_size = 18.0f;
    NVGcolor m_color{};
};

/// 支持 Apple 风格圆角、柔和阴影与描边的容器组件（无 NVG_HOLE 额外指令开销，保障 deko3d 缓冲区安全）。
class StyledSurfaceBox : public brls::Box {
  public:
    explicit StyledSurfaceBox(brls::Axis axis = brls::Axis::COLUMN) : brls::Box(axis) {}

    void setSurfaceColors(NVGcolor fill, NVGcolor border, NVGcolor focusedBorder = nvgRGBA(0, 0, 0, 0)) {
        m_fillColor = fill;
        m_borderColor = border;
        m_focusedBorderColor = focusedBorder;
    }

    void setSurfaceRadius(float radius) {
        m_radius = radius;
        setCornerRadius(radius);
    }
    void setSurfaceBorderWidth(float width) { m_borderWidth = width; }
    void setDrawBorderAfterChildren(bool after) { m_drawBorderAfterChildren = after; }

    void setSurfaceShadow(bool enabled,
                          NVGcolor shadowColor,
                          NVGcolor focusedShadowColor = nvgRGBA(0, 0, 0, 0),
                          float offsetY = 4.0f,
                          float feather = 14.0f,
                          float spread = 1.0f) {
        m_drawShadow = enabled;
        m_shadowColor = shadowColor;
        m_focusedShadowColor = focusedShadowColor;
        m_shadowOffsetY = offsetY;
        m_shadowFeather = feather;
        m_shadowSpread = spread;
    }

    void setFocusAccentLabel(brls::Label* label, NVGcolor normalColor, NVGcolor focusedColor) {
        m_accentLabel = label;
        m_normalLabelColor = normalColor;
        m_focusedLabelColor = focusedColor;
        if (m_accentLabel) {
            m_accentLabel->setTextColor(isFocused() ? m_focusedLabelColor : m_normalLabelColor);
        }
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        if (m_accentLabel) {
            m_accentLabel->setTextColor(m_focusedLabelColor);
        }
    }

    void onFocusLost() override {
        brls::Box::onFocusLost();
        if (m_accentLabel) {
            m_accentLabel->setTextColor(m_normalLabelColor);
        }
    }

    void draw(NVGcontext* vg,
              float x,
              float y,
              float width,
              float height,
              brls::Style style,
              brls::FrameContext* ctx) override {
        if (width <= 0.5f || height <= 0.5f) {
            brls::Box::draw(vg, x, y, width, height, style, ctx);
            return;
        }

        const bool focusedNow = isFocused();
        const float r = std::clamp(m_radius, 0.0f, std::min(width, height) * 0.5f);

        nvgSave(vg);
        if (m_drawShadow) {
            const NVGcolor rawSh =
                (focusedNow && m_focusedShadowColor.a > 0.001f) ? m_focusedShadowColor : m_shadowColor;
            const NVGcolor shCol = a(rawSh);
            if (shCol.a > 0.001f) {
                const float spread = focusedNow ? (m_shadowSpread + 1.5f) : m_shadowSpread;
                const float feather = focusedNow ? (m_shadowFeather + 3.0f) : m_shadowFeather;
                const float offY = focusedNow ? (m_shadowOffsetY + 1.5f) : m_shadowOffsetY;
                const float pad = spread + feather * 0.65f;
                const NVGpaint shPaint = nvgBoxGradient(
                    vg,
                    x - spread,
                    y - spread + offY,
                    width + spread * 2.0f,
                    height + spread * 2.0f,
                    r + spread,
                    feather,
                    shCol,
                    nvgRGBA(0, 0, 0, 0));
                nvgBeginPath(vg);
                nvgRoundedRect(vg,
                               x - pad,
                               y - pad + offY,
                               width + pad * 2.0f,
                               height + pad * 2.0f,
                               r + pad * 0.5f);
                nvgFillPaint(vg, shPaint);
                nvgFill(vg);
            }
        }

        // 不透明/半透明填充卡片底色（位于阴影之上）
        const NVGcolor fill = a(m_fillColor);
        if (fill.a > 0.001f) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, r);
            nvgFillColor(vg, fill);
            nvgFill(vg);
        }

        auto drawStroke = [&]() {
            if (m_borderWidth <= 0.0f) {
                return;
            }
            const NVGcolor rawBr =
                (focusedNow && m_focusedBorderColor.a > 0.001f) ? m_focusedBorderColor : m_borderColor;
            const NVGcolor br = a(rawBr);
            if (br.a <= 0.001f) {
                return;
            }
            const float bw = (focusedNow && m_focusedBorderColor.a > 0.001f)
                                 ? std::max(m_borderWidth, 1.5f)
                                 : m_borderWidth;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x + 0.5f, y + 0.5f, width - 1.0f, height - 1.0f, std::max(0.0f, r - 0.5f));
            nvgStrokeWidth(vg, bw);
            nvgStrokeColor(vg, br);
            nvgStroke(vg);
        };

        if (!m_drawBorderAfterChildren) {
            drawStroke();
        }
        nvgRestore(vg);

        brls::Box::draw(vg, x, y, width, height, style, ctx);

        if (m_drawBorderAfterChildren && m_borderWidth > 0.0f) {
            nvgSave(vg);
            drawStroke();
            nvgRestore(vg);
        }
    }

  private:
    NVGcolor m_fillColor = nvgRGBA(0, 0, 0, 0);
    NVGcolor m_borderColor = nvgRGBA(0, 0, 0, 0);
    NVGcolor m_focusedBorderColor = nvgRGBA(0, 0, 0, 0);
    NVGcolor m_shadowColor = nvgRGBA(0, 0, 0, 0);
    NVGcolor m_focusedShadowColor = nvgRGBA(0, 0, 0, 0);

    float m_radius = 14.0f;
    float m_borderWidth = 1.0f;
    bool m_drawShadow = false;
    bool m_drawBorderAfterChildren = true;
    float m_shadowOffsetY = 4.0f;
    float m_shadowFeather = 14.0f;
    float m_shadowSpread = 1.0f;

    brls::Label* m_accentLabel = nullptr;
    NVGcolor m_normalLabelColor{};
    NVGcolor m_focusedLabelColor{};
};

} // namespace levelup::ui
