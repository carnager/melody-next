// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>

#include <algorithm>

namespace trackknife::bench {

// The command palette's match: every word typed appears, in any case, in
// the command's name, its shortcut or its id. Both windows' palettes use it.
[[nodiscard]] inline bool commandMatches(const QString& filter, const QString& name,
                                         const QString& shortcut, const QString& id) {
    const auto words = filter.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const auto haystack = name + QLatin1Char(' ') + shortcut + QLatin1Char(' ') + id;
    return std::ranges::all_of(
        words, [&](const QString& word) { return haystack.contains(word, Qt::CaseInsensitive); });
}

} // namespace trackknife::bench
