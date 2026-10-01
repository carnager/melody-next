// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/color_scheme.hpp"

#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QSettings>
#include <QStyleHints>

#include <cstring>

namespace trackknife::bench {
namespace {

struct Roles {
    QColor window, window_text, base, alternate_base, text, button, button_text, bright_text,
        highlight, highlighted_text, link, link_visited, placeholder, tooltip_base, tooltip_text,
        light, midlight, mid, dark, shadow, disabled_text, disabled_highlight,
        disabled_highlighted_text;
};

[[nodiscard]] QPalette build(const Roles& roles) {
    QPalette palette;
    for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        palette.setColor(group, QPalette::Window, roles.window);
        palette.setColor(group, QPalette::WindowText, roles.window_text);
        palette.setColor(group, QPalette::Base, roles.base);
        palette.setColor(group, QPalette::AlternateBase, roles.alternate_base);
        palette.setColor(group, QPalette::Text, roles.text);
        palette.setColor(group, QPalette::Button, roles.button);
        palette.setColor(group, QPalette::ButtonText, roles.button_text);
        palette.setColor(group, QPalette::BrightText, roles.bright_text);
        palette.setColor(group, QPalette::Highlight, roles.highlight);
        palette.setColor(group, QPalette::HighlightedText, roles.highlighted_text);
        palette.setColor(group, QPalette::Link, roles.link);
        palette.setColor(group, QPalette::LinkVisited, roles.link_visited);
        palette.setColor(group, QPalette::PlaceholderText, roles.placeholder);
        palette.setColor(group, QPalette::ToolTipBase, roles.tooltip_base);
        palette.setColor(group, QPalette::ToolTipText, roles.tooltip_text);
        palette.setColor(group, QPalette::Light, roles.light);
        palette.setColor(group, QPalette::Midlight, roles.midlight);
        palette.setColor(group, QPalette::Mid, roles.mid);
        palette.setColor(group, QPalette::Dark, roles.dark);
        palette.setColor(group, QPalette::Shadow, roles.shadow);
        palette.setColor(group, QPalette::Accent, roles.highlight);
    }
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, roles.disabled_text);
    }
    palette.setColor(QPalette::Disabled, QPalette::Highlight, roles.disabled_highlight);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText,
                     roles.disabled_highlighted_text);
    return palette;
}

// The desktop's icon theme in the variant a scheme needs: its glyphs light
// on dark, dark on light. Papirus-Dark and Papirus-Light, breeze-dark and
// breeze, Adwaita... -- by the "-dark" and "-light" its family's names carry,
// among the themes installed. The theme itself when it has no such sibling.
[[nodiscard]] bool icon_theme_installed(const QString& name) {
    for (const auto& path : QIcon::themeSearchPaths()) {
        if (QFileInfo::exists(path + QLatin1Char('/') + name + QStringLiteral("/index.theme"))) {
            return true;
        }
    }
    return false;
}

} // namespace

QString iconThemeVariant(const QString& theme, const bool dark) {
    if (theme.isEmpty()) {
        return theme;
    }
    auto family = theme;
    for (const auto* suffix : {"-dark", "-light", "_dark", "_light"}) {
        if (family.endsWith(QLatin1String(suffix), Qt::CaseInsensitive)) {
            family.chop(static_cast<qsizetype>(std::strlen(suffix)));
            break;
        }
    }
    const auto capital = !family.isEmpty() && family.front().isUpper();
    const QStringList candidates =
        dark ? QStringList{family + (capital ? QStringLiteral("-Dark") : QStringLiteral("-dark")),
                           family + QStringLiteral("-dark"), family + QStringLiteral("-Dark")}
             : QStringList{family + (capital ? QStringLiteral("-Light") : QStringLiteral("-light")),
                           family + QStringLiteral("-light"), family + QStringLiteral("-Light"),
                           family};
    for (const auto& candidate : candidates) {
        if (candidate.compare(theme, Qt::CaseInsensitive) == 0 || icon_theme_installed(candidate)) {
            return candidate;
        }
    }
    return theme;
}

namespace {

[[nodiscard]] float luma(const QColor& color) {
    return 0.2126F * color.redF() + 0.7152F * color.greenF() + 0.0722F * color.blueF();
}

} // namespace

ColorScheme colorSchemeNamed(const QString& name) {
    return name == QStringLiteral("light")  ? ColorScheme::light
           : name == QStringLiteral("dark") ? ColorScheme::dark
                                            : ColorScheme::system;
}

QString colorSchemeName(const ColorScheme scheme) {
    switch (scheme) {
    case ColorScheme::light:
        return QStringLiteral("light");
    case ColorScheme::dark:
        return QStringLiteral("dark");
    case ColorScheme::system:
        break;
    }
    return QStringLiteral("system");
}

ColorScheme chosenColorScheme() {
    return colorSchemeNamed(
        QSettings{}.value(QLatin1String(color_scheme_key), QStringLiteral("system")).toString());
}

// Grounds a shade apart, one blue accent, text near white: the mockup's.
QPalette darkPalette() {
    return build(Roles{
        .window = QColor{0x24, 0x27, 0x2c},
        .window_text = QColor{0xe6, 0xe8, 0xeb},
        .base = QColor{0x1b, 0x1d, 0x21},
        .alternate_base = QColor{0x21, 0x24, 0x29},
        .text = QColor{0xe6, 0xe8, 0xeb},
        .button = QColor{0x2e, 0x32, 0x38},
        .button_text = QColor{0xe6, 0xe8, 0xeb},
        .bright_text = QColor{0xff, 0xff, 0xff},
        .highlight = QColor{0x3a, 0x8e, 0xe6},
        .highlighted_text = QColor{0xff, 0xff, 0xff},
        .link = QColor{0x5a, 0xa9, 0xf0},
        .link_visited = QColor{0xa5, 0x8c, 0xf0},
        .placeholder = QColor{0x8b, 0x92, 0x9b},
        .tooltip_base = QColor{0x2e, 0x32, 0x38},
        .tooltip_text = QColor{0xe6, 0xe8, 0xeb},
        .light = QColor{0x3c, 0x41, 0x48},
        .midlight = QColor{0x33, 0x37, 0x3d},
        .mid = QColor{0x1e, 0x21, 0x25},
        .dark = QColor{0x14, 0x16, 0x19},
        .shadow = QColor{0x00, 0x00, 0x00},
        .disabled_text = QColor{0x7c, 0x83, 0x8c},
        .disabled_highlight = QColor{0x3c, 0x41, 0x48},
        .disabled_highlighted_text = QColor{0x9a, 0xa0, 0xa8},
    });
}

// Its counterpart: light grounds, the same accent a shade deeper for white.
QPalette lightPalette() {
    return build(Roles{
        .window = QColor{0xef, 0xf0, 0xf2},
        .window_text = QColor{0x1f, 0x23, 0x28},
        .base = QColor{0xff, 0xff, 0xff},
        .alternate_base = QColor{0xf5, 0xf6, 0xf8},
        .text = QColor{0x1f, 0x23, 0x28},
        .button = QColor{0xe4, 0xe6, 0xe9},
        .button_text = QColor{0x1f, 0x23, 0x28},
        .bright_text = QColor{0xff, 0xff, 0xff},
        .highlight = QColor{0x2f, 0x7f, 0xd6},
        .highlighted_text = QColor{0xff, 0xff, 0xff},
        .link = QColor{0x1f, 0x6f, 0xc5},
        .link_visited = QColor{0x6a, 0x4f, 0xc0},
        .placeholder = QColor{0x7a, 0x81, 0x8a},
        .tooltip_base = QColor{0xff, 0xff, 0xff},
        .tooltip_text = QColor{0x1f, 0x23, 0x28},
        .light = QColor{0xff, 0xff, 0xff},
        .midlight = QColor{0xf2, 0xf3, 0xf5},
        .mid = QColor{0xc9, 0xcc, 0xd1},
        .dark = QColor{0xa5, 0xa9, 0xb0},
        .shadow = QColor{0x6b, 0x6f, 0x75},
        .disabled_text = QColor{0x80, 0x86, 0x8e},
        .disabled_highlight = QColor{0xd5, 0xd8, 0xdc},
        .disabled_highlighted_text = QColor{0x7a, 0x81, 0x8a},
    });
}

ColorSchemes& ColorSchemes::instance() {
    static ColorSchemes schemes;
    return schemes;
}

ColorSchemes::ColorSchemes() = default;

void ColorSchemes::apply(const ColorScheme scheme) {
    scheme_ = scheme;
    if (!desktop_icon_theme_) {
        desktop_icon_theme_ = QIcon::themeName();
    }
    if (scheme == ColorScheme::light) {
        QGuiApplication::setPalette(lightPalette());
        own_ = true;
    } else if (scheme == ColorScheme::dark) {
        QGuiApplication::setPalette(darkPalette());
        own_ = true;
    } else {
        // The desktop's own, unset, to follow it; the dark scheme only
        // where the desktop says dark and hands over a light palette.
        QGuiApplication::setPalette(QPalette{});
        own_ = false;
        const auto* hints = QGuiApplication::styleHints();
        if (hints != nullptr && hints->colorScheme() == Qt::ColorScheme::Dark &&
            luma(QGuiApplication::palette().color(QPalette::Window)) > 0.5F) {
            QGuiApplication::setPalette(darkPalette());
            own_ = true;
        }
    }
    // Icons to match: the desktop's own theme with its colours, else its
    // variant for the scheme in use.
    const auto icon_theme =
        !own_ ? *desktop_icon_theme_
              : iconThemeVariant(*desktop_icon_theme_,
                               QGuiApplication::palette().color(QPalette::Window).lightness() <
                                   128);
    if (icon_theme != QIcon::themeName()) {
        QIcon::setThemeName(icon_theme);
        ++icon_revision_;
    }
    if (!following_) {
        following_ = true;
        if (const auto* hints = QGuiApplication::styleHints(); hints != nullptr) {
            connect(hints, &QStyleHints::colorSchemeChanged, this, [this] {
                if (scheme_ == ColorScheme::system) {
                    apply(ColorScheme::system);
                }
            });
        }
    }
    emit applied();
}

} // namespace trackknife::bench
