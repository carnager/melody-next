// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_open_list.hpp"

namespace trackknife::quick {

QuickOpenList::QuickOpenList(bench::OpenListSession* session, QObject* parent)
    : QObject(parent), session_(session) {
    session_->setParent(this);
    connect(session_, &bench::OpenListSession::changed, this, &QuickOpenList::changed);
    connect(session_, &bench::OpenListSession::opened, this, &QuickOpenList::opened);
}

QVariantList QuickOpenList::groups() const {
    QVariantList groups;
    for (const auto& group : session_->groups()) {
        QVariantList lists;
        for (const auto& list : group.lists) {
            lists.append(QVariantMap{{QStringLiteral("label"), list.label},
                                     {QStringLiteral("tracks"), list.tracks}});
        }
        groups.append(QVariantMap{{QStringLiteral("name"), group.name},
                                  {QStringLiteral("note"), group.note},
                                  {QStringLiteral("lists"), lists}});
    }
    return groups;
}

} // namespace trackknife::quick
