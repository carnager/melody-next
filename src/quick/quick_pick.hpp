// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/quick_pick_session.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

namespace trackknife::quick {

// Quick album and Quick track in the Qt Quick window: a QuickPickSession,
// as QML draws it.
class QuickPick final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Opened by the workspace")
    Q_PROPERTY(bool albums READ albums CONSTANT)
    Q_PROPERTY(QString scope READ scope CONSTANT)
    Q_PROPERTY(QString placeholder READ placeholder CONSTANT)
    Q_PROPERTY(QString keys READ keys CONSTANT)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)

  public:
    explicit QuickPick(bench::QuickPickSession* session, QObject* parent = nullptr);

    [[nodiscard]] bool albums() const { return session_->kind() == bench::QuickPickKind::album; }
    [[nodiscard]] QString scope() const { return session_->scope(); }
    [[nodiscard]] QString placeholder() const { return session_->placeholder(); }
    [[nodiscard]] static QString keys() { return bench::QuickPickSession::keysText(); }
    [[nodiscard]] QVariantList rows() const;
    [[nodiscard]] QString status() const { return session_->status(); }

    Q_INVOKABLE void setText(const QString& text) { session_->setText(text); }
    Q_INVOKABLE void search() { session_->search(); }
    // Enter on `row`, with the modifiers held.
    Q_INVOKABLE void choose(int row, int modifiers);
    Q_INVOKABLE void release() { deleteLater(); }

  signals:
    void changed();
    // Chosen: the popup is done.
    void chosen();

  private:
    bench::QuickPickSession* session_;
};

} // namespace trackknife::quick
