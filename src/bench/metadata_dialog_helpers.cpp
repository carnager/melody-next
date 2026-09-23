// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_dialog_helpers.hpp"

#include "trackknife/core/local_sources.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_apply.hpp"

#include <QByteArrayView>
#include <QStringList>

#include <algorithm>
#include <ranges>
#include <utility>

namespace trackknife::bench {

QString display_utf8(const std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString display_raw_path(const std::string_view value) {
    return display_utf8(core::display_raw_path(value));
}

std::string encode_utf8(const QString& value) {
    const auto encoded = value.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

QString pluralized(const std::size_t count, const QString& singular, const QString& plural) {
    return count == 1U ? singular : plural;
}

QString display_plan_values(const std::vector<std::string>& values) {
    constexpr auto maximum_visible_values = std::size_t{8U};
    constexpr auto maximum_visible_characters = qsizetype{512};
    QStringList visible;
    visible.reserve(static_cast<qsizetype>(std::min(values.size(), maximum_visible_values) +
                                           (values.size() > maximum_visible_values)));
    for (const auto& value : values | std::views::take(maximum_visible_values)) {
        auto display = value.empty() ? QStringLiteral("(empty value)") : display_utf8(value);
        if (display.size() > maximum_visible_characters) {
            display = display.left(maximum_visible_characters) + QChar{0x2026};
        }
        visible.push_back(std::move(display));
    }
    if (values.size() > maximum_visible_values) {
        visible.push_back(
            QStringLiteral("… +%1 values").arg(values.size() - maximum_visible_values));
    }
    return visible.join(QStringLiteral("  ·  "));
}

QString apply_state_text(const operations::MetadataApplySourceState state) {
    using State = operations::MetadataApplySourceState;
    switch (state) {
    case State::pending:
        return QStringLiteral("Waiting");
    case State::running:
        return QStringLiteral("Saving");
    case State::committed:
        return QStringLiteral("Saved");
    case State::failed:
        return QStringLiteral("Failed");
    case State::cancelled:
        return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Unknown");
}

QString file_apply_state_text(const operations::FilePublicationApplySourceState state) {
    using State = operations::FilePublicationApplySourceState;
    switch (state) {
    case State::pending:
        return QStringLiteral("Waiting");
    case State::running:
        return QStringLiteral("Changing");
    case State::unchanged:
        return QStringLiteral("Unchanged");
    case State::committed:
        return QStringLiteral("Changed");
    case State::failed:
        return QStringLiteral("Failed");
    case State::cancelled:
        return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Unknown");
}

} // namespace trackknife::bench
