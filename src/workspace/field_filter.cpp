// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/field_filter.hpp"

#include "bench/metadata_grid_model.hpp"

namespace trackknife::bench {

FieldFilterOutcome filterFields(const MetadataAggregateModel& model, const FieldFilter& filter) {
    // Wait for the existing bounded worker projection instead of traversing tracks.
    const auto ready = model.summaryReady() && model.draftPreviewReady();
    if (filter.changed_only && !ready) {
        return {.hidden = std::nullopt,
                .status =
                    QStringLiteral("Updating changed fields… · Apply includes hidden edits.")};
    }
    const auto query = filter.query.trimmed();
    std::vector<bool> hidden(static_cast<std::size_t>(model.rowCount()), false);
    int visible = 0;
    int changed = 0;
    for (int row = 0; row < model.rowCount(); ++row) {
        const auto field = model.index(row, 0);
        const auto staged = model.index(row, 2).data(metadata_cell_staged_role).toBool();
        changed += staged ? 1 : 0;
        const auto canonical = field.data(metadata_field_canonical_name_role).toString();
        const auto matches = field.data().toString().contains(query, Qt::CaseInsensitive) ||
                             canonical.contains(query, Qt::CaseInsensitive);
        const auto in_layout = filter.layout_fields.isEmpty() ||
                               filter.layout_fields.contains(canonical, Qt::CaseInsensitive);
        const auto hide = !in_layout || !matches || (filter.changed_only && !staged);
        hidden[static_cast<std::size_t>(row)] = hide;
        visible += hide ? 0 : 1;
    }
    const auto counts = QStringLiteral("%1 of %2 fields shown").arg(visible).arg(model.rowCount());
    return {.hidden = std::move(hidden),
            .status = ready
                          ? QStringLiteral(
                                "%1 · %2 changed in selected files · Apply includes hidden edits.")
                                .arg(counts)
                                .arg(changed)
                          : QStringLiteral("%1 · Updating changes… · Apply includes hidden edits.")
                                .arg(counts)};
}

} // namespace trackknife::bench
