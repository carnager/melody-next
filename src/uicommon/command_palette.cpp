// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/command_palette.hpp"
#include "workspace/command_search.hpp"

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>

namespace trackknife::ui {
namespace {
constexpr int shortcut_role = Qt::UserRole + 1;

class CommandDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& supplied,
               const QModelIndex& index) const override {
        QStyleOptionViewItem option(supplied);
        initStyleOption(&option, index);
        const auto shortcut = index.data(shortcut_role).toString();
        if (shortcut.isEmpty()) {
            QStyledItemDelegate::paint(painter, supplied, index);
            return;
        }
        const auto* style = option.widget ? option.widget->style() : QApplication::style();
        const auto text_rect =
            style->subElementRect(QStyle::SE_ItemViewItemText, &option, option.widget)
                .adjusted(4, 0, -8, 0);
        const auto shortcut_width = option.fontMetrics.horizontalAdvance(shortcut);
        const auto label_width = std::max(0, text_rect.width() - shortcut_width - 24);
        option.text = option.fontMetrics.elidedText(option.text, Qt::ElideRight, label_width);
        // Keep the full item rect for selection/focus styling; only the label is elided.
        style->drawControl(QStyle::CE_ItemViewItem, &option, painter, option.widget);
        painter->save();
        painter->setFont(option.font);
        const auto group =
            option.state & QStyle::State_Enabled ? QPalette::Active : QPalette::Disabled;
        const auto role =
            option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text;
        painter->setPen(option.palette.color(group, role));
        painter->setClipRect(text_rect);
        painter->drawText(text_rect, Qt::AlignRight | Qt::AlignVCenter, shortcut);
        painter->restore();
    }
};

QString commandName(const QAction* action) {
    auto name = action->text();
    name.remove(QLatin1Char('&'));
    return name.trimmed();
}

// What the list shows: the command, under the submenus it is in -- not the
// menu bar's own menu, which only the search looks at.
QString shownName(const PaletteCommand& command) {
    auto parts = command.path.mid(1);
    parts.append(commandName(command.action));
    return parts.join(QStringLiteral(" › "));
}

void collect(const QList<QAction*>& actions, const QStringList& path,
             const QList<QPointer<QAction>>& menus, QList<PaletteCommand>& into,
             QSet<const QAction*>& seen) {
    for (auto* action : actions) {
        // Separators, and headings drawn as widgets, are not commands.
        if (action == nullptr || action->isSeparator() ||
            qobject_cast<QWidgetAction*>(action) != nullptr || seen.contains(action)) {
            continue;
        }
        seen.insert(action);
        if (const auto* menu = action->menu(); menu != nullptr) {
            auto below = menus;
            below.append(action);
            collect(menu->actions(), path + QStringList{commandName(action)}, below, into, seen);
            continue;
        }
        // The palette's own entry would only open it again.
        if (commandName(action).isEmpty() ||
            action->objectName() == QStringLiteral("action-command-palette")) {
            continue;
        }
        into.append(PaletteCommand{.action = action, .path = path, .menus = menus});
    }
}
} // namespace

QList<PaletteCommand> windowCommands(const QMainWindow& window) {
    QList<PaletteCommand> commands;
    QSet<const QAction*> seen;
    if (const auto* bar = window.menuBar(); bar != nullptr) {
        collect(bar->actions(), {}, {}, commands, seen);
    }
    collect(window.actions(), {}, {}, commands, seen);
    return commands;
}

CommandPalette::CommandPalette(const QList<QAction*>& actions, QWidget* parent)
    : CommandPalette(
          [&actions] {
              QList<PaletteCommand> commands;
              QSet<const QAction*> seen;
              collect(actions, {}, {}, commands, seen);
              return commands;
          }(),
          parent) {}

CommandPalette::CommandPalette(QList<PaletteCommand> commands, QWidget* parent)
    : QDialog(parent), commands_(std::move(commands)) {
    commands_.removeIf([](const PaletteCommand& command) { return !command.action; });
    std::ranges::sort(commands_, {},
                      [](const auto& command) { return shownName(command).toCaseFolded(); });
    setObjectName(QStringLiteral("command-palette"));
    setWindowTitle(tr("Commands"));
    resize(620, 480);

    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("command-filter"));
    filter_->setPlaceholderText(tr("Find a command by name or shortcut…"));
    filter_->setAccessibleName(tr("Command search"));
    filter_->setClearButtonEnabled(true);
    filter_->installEventFilter(this);
    results_ = new QListWidget(this);
    results_->setObjectName(QStringLiteral("command-results"));
    results_->setAccessibleName(tr("Matching commands"));
    results_->setAlternatingRowColors(true);
    results_->setItemDelegate(new CommandDelegate(results_));
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("command-status"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Close, this);
    run_ = buttons_->addButton(tr("Run"), QDialogButtonBox::AcceptRole);
    run_->setObjectName(QStringLiteral("command-run"));
    run_->setDefault(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(filter_);
    layout->addWidget(results_, 1);
    layout->addWidget(status_);
    layout->addWidget(new QLabel(tr("Change shortcuts in Settings → Shortcuts."), this));
    layout->addWidget(buttons_);

    connect(filter_, &QLineEdit::textChanged, this, &CommandPalette::rebuild);
    connect(results_, &QListWidget::currentRowChanged, this, &CommandPalette::updateSelection);
    connect(results_, &QListWidget::itemActivated, this, [this] { runCurrent(); });
    connect(run_, &QPushButton::clicked, this, &CommandPalette::runCurrent);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // A command, or a menu it is in, greyed out, hidden or gone changes
    // what is offered. The pointers clear themselves; indexes stay.
    QSet<const QAction*> watched;
    for (const auto& command : commands_) {
        auto watch = command.menus;
        watch.append(command.action);
        for (const auto& action : watch) {
            if (!action || watched.contains(action.data())) {
                continue;
            }
            watched.insert(action.data());
            connect(action, &QAction::changed, this, &CommandPalette::rebuild);
            connect(action, &QObject::destroyed, this, [this, pointer = action.data()] {
                for (auto& each : commands_) {
                    if (each.action == pointer) {
                        each.action.clear();
                    }
                    for (auto& menu : each.menus) {
                        if (menu == pointer) {
                            menu.clear();
                        }
                    }
                }
                rebuild();
            });
        }
    }
    rebuild();
    filter_->setFocus();
}

void CommandPalette::rebuild() {
    const QPointer<QAction> selected = currentAction();
    results_->clear();
    int selected_row = -1;
    for (qsizetype index = 0; index < commands_.size(); ++index) {
        const auto& command = commands_[index];
        if (!available(command))
            continue;
        const auto& action = command.action;
        const auto shortcut = action->shortcut().toString(QKeySequence::NativeText);
        auto label = shownName(command);
        // The menu bar's menu is searched too: "playback output" finds the
        // speakers.
        const auto searched = command.path.join(QLatin1Char(' ')) + QLatin1Char(' ') + label;
        if (!bench::commandMatches(filter_->text(), searched, shortcut, action->objectName()))
            continue;
        if (action->isCheckable() && action->isChecked())
            label += tr(" · On");
        auto* item = new QListWidgetItem(action->icon(), label, results_);
        item->setData(Qt::UserRole, index);
        item->setData(shortcut_role, shortcut);
        item->setData(Qt::AccessibleTextRole, label + QLatin1Char(' ') + shortcut);
        item->setToolTip(action->toolTip());
        if (action == selected)
            selected_row = results_->count() - 1;
    }
    if (results_->count() > 0)
        results_->setCurrentRow(selected_row >= 0 ? selected_row : 0);
    updateSelection();
}

QAction* CommandPalette::currentAction() const {
    const auto* item = results_->currentItem();
    if (!item)
        return nullptr;
    const auto index = item->data(Qt::UserRole).toLongLong();
    return index >= 0 && index < commands_.size() ? commands_[index].action.data() : nullptr;
}

bool CommandPalette::available(const PaletteCommand& command) const {
    const auto usable = [](const QPointer<QAction>& action) {
        return action && action->isVisible() && action->isEnabled();
    };
    // A menu gone took its commands with it.
    return usable(command.action) && std::ranges::all_of(command.menus, usable);
}

void CommandPalette::updateSelection() {
    const auto* action = currentAction();
    run_->setEnabled(action && action->isEnabled() && action->isVisible());
    status_->setText(!action ? tr("No matching command")
                     : !action->isEnabled()
                         ? tr("Unavailable for the current selection or connection.")
                     : action->statusTip().isEmpty() ? tr("Press Enter to run this command.")
                                                     : action->statusTip());
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == filter_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
            if (results_->count() > 0)
                results_->setCurrentRow(
                    std::clamp(results_->currentRow() + (key->key() == Qt::Key_Down ? 1 : -1), 0,
                               results_->count() - 1));
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void CommandPalette::runCurrent() {
    const QPointer<QAction> action = currentAction();
    if (!action || !action->isEnabled() || !action->isVisible())
        return;
    accept();
    if (action)
        action->trigger();
}
} // namespace trackknife::ui
