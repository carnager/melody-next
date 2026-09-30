// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/search_session.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>

namespace trackknife::quick {

// Search in the Qt Quick window: a SearchSession, as QML draws it.
class QuickSearch final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Opened by the workspace")
    Q_PROPERTY(QStringList scopes READ scopes CONSTANT)
    Q_PROPERTY(QVariantList presetGroups READ presetGroups CONSTANT)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QVariantMap saved READ saved NOTIFY savedChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)

  public:
    explicit QuickSearch(bench::SearchSession* session, QObject* parent = nullptr);

    [[nodiscard]] bench::SearchSession* session() const { return session_; }
    [[nodiscard]] QStringList scopes() const;
    [[nodiscard]] static QVariantList presetGroups();
    [[nodiscard]] QVariantMap state() const;
    [[nodiscard]] QVariantMap saved() const;
    [[nodiscard]] QVariantList results() const;

    Q_INVOKABLE void setScope(int index) { session_->setScope(index); }
    Q_INVOKABLE void setText(const QString& text) { session_->setText(text); }
    Q_INVOKABLE void setQueryMode(bool on) { session_->setQueryMode(on); }
    Q_INVOKABLE void selectSaved(int index) { session_->selectSaved(index); }
    Q_INVOKABLE void useSaved(int index) { session_->useSaved(index); }
    Q_INVOKABLE void saveAs(const QString& name) { session_->saveAs(name); }
    Q_INVOKABLE void update() { session_->update(); }
    Q_INVOKABLE void rename(const QString& name) { session_->rename(name); }
    Q_INVOKABLE void remove() { session_->remove(); }
    // {kind: "none" | "integer" | "text", title, prompt, value, minimum,
    // maximum, step}.
    Q_INVOKABLE static QVariantMap presetInput(int preset);
    Q_INVOKABLE void usePreset(int preset, const QString& value) {
        session_->usePreset(preset, value);
    }
    // bench::LocalLibraryAction, as a number: 0 append, 1 next, 2 replace,
    // 3 new tab.
    Q_INVOKABLE void openAll(int action);
    Q_INVOKABLE void openRows(const QVariantList& rows, int action);
    Q_INVOKABLE void release() { deleteLater(); }

  signals:
    void changed();
    void savedChanged();
    void resultsChanged();

  private:
    bench::SearchSession* session_;
};

} // namespace trackknife::quick
