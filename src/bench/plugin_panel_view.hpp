// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/plugins.hpp"

#include <QWidget>

class QLabel;
class QVBoxLayout;

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
    void showContent(const PluginPanelContent& content);
    // The line under the panel's name: what it was asked about.
    void setSubject(const QString& subject);

  signals:
    void refreshRequested();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void clearParts();

    QLabel* subject_{nullptr};
    QWidget* parts_{nullptr};
    QVBoxLayout* parts_layout_{nullptr};
};

} // namespace trackknife::bench
