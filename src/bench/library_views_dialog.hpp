// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_key.hpp"
#include "workspace/library_view_definitions.hpp"

#include <QDialog>
#include <QTimer>

#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;
class QToolButton;
class QTreeView;

namespace trackknife::bench {

class CatalogueSource;
class LibraryBrowser;

// ADR-0254: makes, changes and removes library views, each a list of
// tkfmt-1 levels, with the library previewed in the view being edited. The
// shipped views can be looked at and copied, not changed.
class LibraryViewsDialog final : public QDialog {
    Q_OBJECT

  public:
    LibraryViewsDialog(const CatalogueSource& catalogues, EngineKey engine, QWidget* parent);

    // Shows the view `id` in the editor.
    void selectView(const QString& id);

  private:
    void loadViews();
    void showView(int row);
    void showLevels();
    void readEdits();
    void edited();
    void preview();
    void addView(bool copy);
    void removeView();
    void addLevel();
    void removeLevel();
    void moveLevel(int by);
    void save();
    [[nodiscard]] LibraryViewDefinition* current();

    std::vector<LibraryViewDefinition> views_;
    int shown_{-1};
    bool dirty_{false};
    bool filling_{false};
    QListWidget* list_{nullptr};
    QPushButton* copy_{nullptr};
    QPushButton* remove_{nullptr};
    QLineEdit* name_{nullptr};
    QTableWidget* levels_{nullptr};
    QToolButton* add_level_{nullptr};
    QToolButton* remove_level_{nullptr};
    QToolButton* level_up_{nullptr};
    QToolButton* level_down_{nullptr};
    QLabel* note_{nullptr};
    QLabel* error_{nullptr};
    QTreeView* preview_view_{nullptr};
    LibraryBrowser* preview_{nullptr};
    QPushButton* save_{nullptr};
    QTimer preview_timer_;
};

} // namespace trackknife::bench
