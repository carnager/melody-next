// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/persistence/list_repository.hpp"

#include <QHash>
#include <QString>

#include <compare>
#include <functional>
#include <string>

namespace trackknife::ui {

// ADR-0234: which engine something belongs to, within this window: this
// computer's ("local"), or another by the id it keeps -- or "remote", the
// configured remote before it has been reached and said who it is. Code that
// needs an engine is handed its key rather than a flag.
class EngineKey final {
  public:
    EngineKey() = default;
    [[nodiscard]] static EngineKey local() { return EngineKey{QStringLiteral("local")}; }
    [[nodiscard]] static EngineKey remote() { return EngineKey{QStringLiteral("remote")}; }
    // The engine a list belongs to, as the list records it.
    [[nodiscard]] static EngineKey of(const persistence::ListDocument& document) {
        return document.engine.empty() ? local()
                                       : EngineKey{QString::fromStdString(document.engine)};
    }
    // As a list records it: empty for this computer's.
    [[nodiscard]] std::string stored() const {
        return isLocal() ? std::string{} : value_.toStdString();
    }
    // Read back from text written by text().
    [[nodiscard]] static EngineKey fromText(const QString& text) { return EngineKey{text}; }

    [[nodiscard]] bool isLocal() const { return value_ == QStringLiteral("local"); }
    [[nodiscard]] bool isNull() const { return value_.isEmpty(); }
    [[nodiscard]] const QString& text() const noexcept { return value_; }

    friend bool operator==(const EngineKey&, const EngineKey&) = default;
    friend std::strong_ordering operator<=>(const EngineKey& left, const EngineKey& right) {
        const auto compared = QString::compare(left.value_, right.value_);
        return compared < 0   ? std::strong_ordering::less
               : compared > 0 ? std::strong_ordering::greater
                              : std::strong_ordering::equal;
    }

  private:
    explicit EngineKey(QString value) : value_(std::move(value)) {}
    QString value_;
};

} // namespace trackknife::ui

template <> struct std::hash<trackknife::ui::EngineKey> {
    std::size_t operator()(const trackknife::ui::EngineKey& key) const noexcept {
        return qHash(key.text());
    }
};
