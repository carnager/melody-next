// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/script_session.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace trackknife::quick {

// The tagging script editor in the Qt Quick window: a ScriptSession, as QML
// draws it.
class QuickScript final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Opened by a tag editor")
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QStringList savedNames READ savedNames NOTIFY savedChanged)
    Q_PROPERTY(int savedIndex READ savedIndex NOTIFY savedChanged)
    Q_PROPERTY(QStringList steps READ steps NOTIFY stepsChanged)
    Q_PROPERTY(QString rawSource READ rawSource NOTIFY rawChanged)
    Q_PROPERTY(QAbstractItemModel* preview READ preview NOTIFY previewChanged)
    Q_PROPERTY(QVariantList stepKinds READ stepKinds CONSTANT)
    Q_PROPERTY(int initialStepKind READ initialStepKind CONSTANT)
    Q_PROPERTY(QStringList captureSources READ captureSources CONSTANT)
    Q_PROPERTY(QStringList ratingScales READ ratingScales CONSTANT)

  public:
    explicit QuickScript(bench::ScriptSession* session, QObject* parent = nullptr);

    [[nodiscard]] QVariantMap state() const;
    [[nodiscard]] QStringList savedNames() const { return session_->savedNames(); }
    [[nodiscard]] int savedIndex() const { return session_->savedIndex(); }
    [[nodiscard]] QStringList steps() const { return session_->steps(); }
    [[nodiscard]] QString rawSource() const { return session_->rawSource(); }
    [[nodiscard]] QAbstractItemModel* preview() const { return session_->preview(); }
    [[nodiscard]] static QVariantList stepKinds();
    [[nodiscard]] static int initialStepKind() { return bench::ScriptSession::initialStepKind(); }
    [[nodiscard]] static QStringList captureSources() {
        return bench::ScriptSession::captureSources();
    }
    [[nodiscard]] static QStringList ratingScales() { return bench::ScriptSession::ratingScales(); }

    Q_INVOKABLE [[nodiscard]] static QVariantMap stepForm(int kind, int capture_source);
    Q_INVOKABLE [[nodiscard]] QStringList targetSuggestions(const QString& query) const {
        return session_->targetSuggestions(query);
    }
    Q_INVOKABLE [[nodiscard]] QStringList fieldListSuggestions(const QString& text) const {
        return session_->fieldListSuggestions(text);
    }
    Q_INVOKABLE [[nodiscard]] static QString completeFieldList(const QString& text,
                                                               const QString& name) {
        return bench::ScriptSession::completeFieldList(text, name);
    }
    Q_INVOKABLE [[nodiscard]] static QVariantMap translateRuleScript(const QString& source);
    Q_INVOKABLE [[nodiscard]] bool canRemove(int row) const { return session_->canRemove(row); }
    Q_INVOKABLE [[nodiscard]] bool canMoveUp(int row) const { return session_->canMoveUp(row); }
    Q_INVOKABLE [[nodiscard]] bool canMoveDown(int row) const { return session_->canMoveDown(row); }
    Q_INVOKABLE [[nodiscard]] QString suggestedExportName() const {
        return session_->suggestedExportName();
    }

    Q_INVOKABLE void selectSaved(int index) { session_->selectSaved(index); }
    Q_INVOKABLE void setName(const QString& name) { session_->setName(name); }
    // What to focus next: "target", "input", "argument", "name" or "".
    Q_INVOKABLE QString addStep(const QVariantMap& step);
    Q_INVOKABLE void removeStep(int row) { session_->removeStep(row); }
    Q_INVOKABLE void moveStep(int row, int offset) { session_->moveStep(row, offset); }
    Q_INVOKABLE void setRawSource(const QString& source) { session_->setRawSource(source); }
    Q_INVOKABLE void importRuleScript(const QString& source, bool append) {
        session_->importRuleScript(source, append);
    }
    Q_INVOKABLE void importNative(const QUrl& file) { session_->importNative(file.toLocalFile()); }
    Q_INVOKABLE void exportNative(const QUrl& file) { session_->exportNative(file.toLocalFile()); }
    Q_INVOKABLE QString save(bool as_new);
    Q_INVOKABLE void deleteSaved() { session_->deleteSaved(); }
    Q_INVOKABLE void stage() { session_->stage(); }
    // "close", "confirm" or "wait".
    Q_INVOKABLE QString requestClose();
    // Its window closed: gone.
    Q_INVOKABLE void release() { deleteLater(); }

  signals:
    void changed();
    void savedChanged();
    void stepsChanged(int select);
    void rawChanged();
    void previewChanged();
    void accepted();
    void closeRequested();

  private:
    [[nodiscard]] static QString focusName(bench::ScriptSession::Focus focus);

    bench::ScriptSession* session_;
};

} // namespace trackknife::quick
