// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QPalette>
#include <QString>

namespace trackknife::bench {

// ADR-0247: Trackknife's colours -- the system's, or one of its own two
// schemes, light and dark, the same on every desktop.
enum class ColorScheme { system, light, dark };

// Settings › General › Appearance, as stored: "system", "light", "dark".
inline constexpr auto color_scheme_key = "appearance/color-scheme";

[[nodiscard]] ColorScheme colorSchemeNamed(const QString& name);
[[nodiscard]] QString colorSchemeName(ColorScheme scheme);
// What is chosen in Settings now.
[[nodiscard]] ColorScheme chosenColorScheme();

// The two schemes, every role and every state. Inactive is Active: an
// unfocused window keeps its accent.
[[nodiscard]] QPalette lightPalette();
[[nodiscard]] QPalette darkPalette();

// Applies the scheme to the application: its own palettes as chosen; for
// "system", the desktop's -- unless the desktop says it is dark and hands
// over a light palette, as a desktop without Qt integration does, when the
// dark scheme stands in. Announces it, and follows the desktop changing
// between light and dark while "system" is chosen.
class ColorSchemes final : public QObject {
    Q_OBJECT

  public:
    [[nodiscard]] static ColorSchemes& instance();

    void apply(ColorScheme scheme);
    // Whatever Settings holds now.
    void applyChosen() { apply(chosenColorScheme()); }
    [[nodiscard]] ColorScheme scheme() const { return scheme_; }
    // Whether Trackknife's own palette is in use -- the scheme chosen, or
    // the dark one standing in for a desktop that gave none.
    [[nodiscard]] bool ownPalette() const { return own_; }

  signals:
    void applied();

  private:
    ColorSchemes();
    ColorScheme scheme_{ColorScheme::system};
    bool own_{false};
    bool following_{false};
};

} // namespace trackknife::bench
