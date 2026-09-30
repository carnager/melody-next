// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/open_list_session.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

namespace trackknife::quick {

// "Open list…" in the Qt Quick window: an OpenListSession, as QML draws it.
class QuickOpenList final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Opened by the workspace")
    Q_PROPERTY(QVariantList groups READ groups NOTIFY changed)

  public:
    explicit QuickOpenList(bench::OpenListSession* session, QObject* parent = nullptr);

    // Each engine: {name, note, lists: [{label, tracks}]}.
    [[nodiscard]] QVariantList groups() const;

    Q_INVOKABLE void open(int group, int row) { session_->open(group, row); }
    Q_INVOKABLE void release() { deleteLater(); }

  signals:
    void changed();
    void opened();

  private:
    bench::OpenListSession* session_;
};

} // namespace trackknife::quick
