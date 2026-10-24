// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/plugins.hpp"

#include <QHash>
#include <QJsonObject>
#include <QWidget>

class QLabel;
class QLineEdit;
class QVBoxLayout;

class QScrollArea;

namespace trackknife::bench {

// ADR-0291: a plugin's side panel, as the Lyrics panel looks: its name and
// what it is about at the top, then the parts it answered, drawn by Melody --
// headings, paragraphs, items with their detail beneath and a link, images.
class PluginPanelView final : public QWidget {
    Q_OBJECT

  public:
    explicit PluginPanelView(const QString& label, QWidget* parent = nullptr);

    // A word in place of an answer: nothing playing, on its way, or why not.
    void showMessage(const QString& message);
    // `keep_place`: an answer about what it showed already -- asked again --
    // shown where it was scrolled to.
    void showContent(const PluginPanelContent& content, bool keep_place = false);
    // The line under the panel's name: what it was asked about.
    void setSubject(const QString& subject);
    // Whether it shows an answer -- kept while it is asked again.
    [[nodiscard]] bool hasContent() const noexcept { return has_content_; }
    // ADR-0303: every input's text as it stands, by its key.
    [[nodiscard]] QJsonObject inputs() const;

  signals:
    void refreshRequested();
    // ADR-0303: a button or a toggle pressed -- its action, or a toggle's
    // key, and its value ("on" or "off" for a toggle).
    void actionRequested(const QString& action, const QString& value);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void clearParts();

    QLabel* subject_{nullptr};
    QScrollArea* scroll_{nullptr};
    QWidget* parts_{nullptr};
    QVBoxLayout* parts_layout_{nullptr};
    bool has_content_{false};
    // ADR-0303: the inputs shown, and what was typed in them -- kept over an
    // answer that gives the same value as the last, so typing survives the
    // panel being asked again; one the plugin changes takes the plugin's.
    QHash<QString, QLineEdit*> inputs_;
    QHash<QString, QString> typed_;
    QHash<QString, QString> given_;
};

} // namespace trackknife::bench
