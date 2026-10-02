// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/persistence/local_library.hpp"
#include "workspace/library_browser.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QTimer>

#include <memory>
#include <vector>

namespace trackknife::engine {
class Catalogue;
}

namespace trackknife::bench {

enum class QuickPickKind { album, track };

// Quick album and Quick track: an album or a track found from the keyboard
// -- every word typed must appear in its artist, title, album or date
// ("doors 67"), the newest first with nothing typed -- and put somewhere as
// the library's own menu would. The window's popup draws it.
class QuickPickSession final : public QObject {
    Q_OBJECT

  public:
    struct Row {
        // The album, or the track's title.
        QString name;
        // Who, when, how long -- or who, from what, when.
        QString details;
    };

    // `catalogue` is the library searched, named `scope`.
    QuickPickSession(QuickPickKind kind, std::shared_ptr<engine::Catalogue> catalogue,
                     QString scope, QObject* parent = nullptr);
    ~QuickPickSession() override;

    [[nodiscard]] QuickPickKind kind() const { return kind_; }
    [[nodiscard]] QString scope() const { return scope_; }
    [[nodiscard]] QString placeholder() const;
    [[nodiscard]] static QString keysText();
    [[nodiscard]] const std::vector<Row>& rows() const { return rows_; }
    [[nodiscard]] QString status() const { return status_; }

    // Typed: searched once typing pauses.
    void setText(const QString& text);
    // Searched now, with what is typed.
    void search();
    void choose(int row, LocalLibraryAction action);
    // The action a key's modifiers ask for: Enter add, Shift replace and
    // play, Ctrl play next, Ctrl+Shift add to Up Next, Alt a new tab.
    [[nodiscard]] static LocalLibraryAction actionFor(Qt::KeyboardModifiers modifiers);

  signals:
    void changed();
    void chosen(std::vector<persistence::LibraryEntry> picked, LocalLibraryAction action);

  private:
    struct Found {
        std::vector<persistence::LibraryEntry> entries;
        bool more{false};
        // The newest in the library, asked for with nothing typed.
        bool newest{false};
        QString error;
        quint64 generation{0};
    };
    void showResults();
    [[nodiscard]] QString idleText() const;

    const QuickPickKind kind_;
    std::shared_ptr<engine::Catalogue> catalogue_;
    QString scope_;
    QString text_;
    QString status_;
    std::vector<Row> rows_;
    QTimer debounce_;
    QFutureWatcher<Found> watcher_;
    quint64 generation_{0};
    bool pending_{false};
    std::vector<persistence::LibraryEntry> entries_;
};

} // namespace trackknife::bench
