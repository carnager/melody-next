// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace trackknife::bench {

class MetadataAggregateModel;

// What the tag editor's field list shows: fields whose display or canonical
// name contains the query, only those with staged edits when asked, only a
// field set's when one is chosen. Presentation only -- hidden fields keep
// their edits, and Apply writes them.
struct FieldFilter {
    QString query;
    bool changed_only{false};
    QStringList layout_fields;
};

struct FieldFilterOutcome {
    // Per field row, whether it is hidden; none while the changed fields are
    // still being worked out and only they are to be shown.
    std::optional<std::vector<bool>> hidden;
    QString status;
};

[[nodiscard]] FieldFilterOutcome filterFields(const MetadataAggregateModel& model,
                                              const FieldFilter& filter);

} // namespace trackknife::bench
