// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/persistence/list_repository.hpp"
#include <QWidget>
#include <functional>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QLabel;
class QPushButton;

namespace trackknife::bench {
class ConnectionProfilesWidget final : public QWidget {
  public:
    using Profiles = std::vector<persistence::ConnectionProfile>;
    using Writer = std::function<void(Profiles, std::function<void(QString)>)>;
    ConnectionProfilesWidget(Profiles profiles, Writer writer, QWidget* parent = nullptr);

  private:
    void rebuild(int selected);
    void select(int row);
    void persist(bool remove);
    void updateButtons();
    Profiles profiles_;
    Writer writer_;
    bool saving_{false};
    QComboBox* selector_;
    QLineEdit* name_;
    QLineEdit* host_;
    QSpinBox* port_;
    QLineEdit* root_;
    QCheckBox* startup_;
    QLabel* status_;
    QPushButton* save_;
    QPushButton* remove_;
};
} // namespace trackknife::bench
