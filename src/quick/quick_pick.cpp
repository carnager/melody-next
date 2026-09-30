// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_pick.hpp"

namespace trackknife::quick {

QuickPick::QuickPick(bench::QuickPickSession* session, QObject* parent)
    : QObject(parent), session_(session) {
    session_->setParent(this);
    connect(session_, &bench::QuickPickSession::changed, this, &QuickPick::changed);
    connect(session_, &bench::QuickPickSession::chosen, this, &QuickPick::chosen);
}

QVariantList QuickPick::rows() const {
    QVariantList rows;
    for (const auto& row : session_->rows()) {
        rows.append(QVariantMap{{QStringLiteral("name"), row.name},
                                {QStringLiteral("details"), row.details}});
    }
    return rows;
}

void QuickPick::choose(const int row, const int modifiers) {
    session_->choose(row, bench::QuickPickSession::actionFor(
                              static_cast<Qt::KeyboardModifiers>(modifiers)));
}

} // namespace trackknife::quick
